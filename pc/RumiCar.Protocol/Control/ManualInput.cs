namespace RumiCar.Protocol.Control;

/// <summary>Keys of the manual driving (arrow keys in the drive manager).</summary>
public enum ManualKey { Left, Right, Forward, Reverse }

/// <summary>Which input decides the manual command at the moment (for the display).</summary>
public enum ManualSource { None, Pad, Keys, Scroll }

/// <summary>
/// The manual command in RC_steer / RC_drive values (<see cref="RumiCarApi"/>):
/// steer LEFT / CENTER / RIGHT, drive FREE (stop) / FORWARD / REVERSE with a duty 0..255.
/// </summary>
public readonly record struct ManualCommand(int Steer, int DriveDir, int DrivePwm, ManualSource Source)
{
    /// <summary>−1 left, 0 centre, +1 right (for drawing the pad).</summary>
    public int SignedSteer => Steer == RumiCarApi.Left ? -1 : Steer == RumiCarApi.Right ? 1 : 0;
    /// <summary>The duty with the direction as its sign, 0 for FREE (for drawing the pad).</summary>
    public int SignedDrive => DriveDir == RumiCarApi.Forward ? DrivePwm : DriveDir == RumiCarApi.Reverse ? -DrivePwm : 0;

    public static ManualCommand FromSigned(int steer, int drive, ManualSource source) => new(
        steer < 0 ? RumiCarApi.Left : steer > 0 ? RumiCarApi.Right : RumiCarApi.Center,
        drive > 0 ? RumiCarApi.Forward : drive < 0 ? RumiCarApi.Reverse : RumiCarApi.Free,
        Math.Abs(drive),
        source);
}

/// <summary>
/// Turns the PC's pointing devices and keys into the manual_drive command
/// (RC_steer LEFT / CENTER / RIGHT, RC_drive FORWARD / REVERSE / FREE with a
/// duty 0..255), docs 6.2.  Inside, the steering is −1/0/+1 and the drive a
/// signed duty −255..255 (the pad is a plane); <see cref="Command"/> gives the
/// RC form.  Pure computation: the caller passes the time in milliseconds, so
/// it can be tested without a UI.
///
///  - Pad (mouse or trackpad, press and drag): the position decides both.  Left
///    and right thirds steer (the steering is on/off, so a wide centre with
///    hysteresis), up is forward and down is reverse, proportional to the
///    distance from the centre line up to <see cref="MaxDrive"/>, with a dead
///    zone around it.  Releasing stops the car (dead man).
///  - Vertical scroll (wheel or two fingers): a speed set-point that stays, in
///    steps of <see cref="WheelStep"/> per wheel notch.  Going through
///    zero stops at zero, and only a new scroll after a pause of
///    <see cref="ZeroDetentMs"/> goes on, so one long swipe never turns
///    "forward" into "reverse".
///  - Horizontal scroll (two fingers sideways, a tilting wheel, or Shift +
///    wheel): steers while it moves and centres <see cref="SwipeReleaseMs"/>
///    after it stops.
///  - Keys: ← → steer while held, ↑ ↓ drive at <see cref="MaxDrive"/> while held.
///  - <see cref="Stop"/>: everything to zero (Space, middle click, leaving the window).
///
/// Precedence: the pad while pressed; otherwise held keys; otherwise the scroll
/// set-point and the horizontal scroll.  Pressing the pad or ↑ ↓ clears the
/// set-point, so letting go never resumes an older speed.
/// </summary>
public sealed class ManualInput
{
    public const int MinMaxDrive = 26;             // duty 0..255: about a tenth
    public const int Quantum = 1;                  // commands move in steps of 1 (the duty is an integer)
    public const double DriveDeadZone = 0.12;      // pad: |y| at or below this is "stop"
    public const double SteerEnter = 0.33;         // pad: |x| from which it steers ...
    public const double SteerExit = 0.22;          // ... and below which it centres again
    public const int WheelNotch = 120;             // Windows WHEEL_DELTA
    public const int WheelStep = 13;               // set-point change per wheel notch (about 5 % of full duty)
    public const int ZeroDetentMs = 400;           // pause at zero when the set-point crosses it
    public const int SwipeThreshold = 40;          // horizontal scroll needed to steer (a third of a notch)
    public const int SwipeReleaseMs = 250;         // steering centres this long after the last horizontal scroll

    private int _maxDrive = 153;

    private bool _dragging;
    private int _dragSteer;
    private int _dragDrive;

    private bool _keyLeft, _keyRight, _keyForward, _keyReverse;
    private int _lastSteerKey;                     // −1 / +1: the one pressed last wins when both are held
    private int _lastDriveKey;

    private int _setPoint;
    private long _wheelAccumulator;                // scroll not yet turned into set-point steps, in delta × WheelStep (exact integers)
    private int _detentDirection;                  // the direction ignored at zero, 0 = none
    private long _detentUntilMs;

    private int _swipeSteer;
    private double _swipeAccumulator;
    private long _lastSwipeMs;

    /// <summary>Upper limit of the duty the inputs ask for (26..255). The car clamps again with its own max_drive_pwm.</summary>
    public int MaxDrive
    {
        get => _maxDrive;
        set
        {
            _maxDrive = Math.Clamp(value, MinMaxDrive, RumiCarApi.PwmMax);
            _setPoint = Math.Clamp(_setPoint, -_maxDrive, _maxDrive);
            _dragDrive = Math.Clamp(_dragDrive, -_maxDrive, _maxDrive);
        }
    }

    /// <summary>Reverses the horizontal scroll (for touchpads set to the other scrolling direction).</summary>
    public bool InvertHorizontal { get; set; }

    public bool Dragging => _dragging;
    public int SetPoint => _setPoint;

    // --- pad ------------------------------------------------------------------------------

    /// <param name="x">−1 (left edge) .. +1 (right edge)</param>
    /// <param name="y">−1 (bottom edge) .. +1 (top edge)</param>
    public void PadDown(double x, double y)
    {
        _dragging = true;
        _dragSteer = 0;
        _setPoint = 0;
        _wheelAccumulator = 0;
        PadMove(x, y);
    }

    public void PadMove(double x, double y)
    {
        if (!_dragging) return;
        x = Math.Clamp(x, -1.0, 1.0);
        y = Math.Clamp(y, -1.0, 1.0);
        _dragSteer = _dragSteer switch
        {
            0 => x <= -SteerEnter ? -1 : x >= SteerEnter ? 1 : 0,
            -1 => x >= SteerEnter ? 1 : x > -SteerExit ? 0 : -1,
            _ => x <= -SteerEnter ? -1 : x < SteerExit ? 0 : 1,
        };
        _dragDrive = PadDrive(y, _maxDrive);
    }

    public void PadUp()
    {
        _dragging = false;
        _dragSteer = 0;
        _dragDrive = 0;
    }

    /// <summary>Drive for a pad height: 0 within the dead zone, then linear up to ±max, in steps of <see cref="Quantum"/>.</summary>
    public static int PadDrive(double y, int maxDrive)
    {
        var a = Math.Abs(Math.Clamp(y, -1.0, 1.0));
        if (a <= DriveDeadZone) return 0;
        var f = (a - DriveDeadZone) / (1.0 - DriveDeadZone);
        var pwm = (int)Math.Round(f * maxDrive / Quantum, MidpointRounding.AwayFromZero) * Quantum;
        pwm = Math.Clamp(pwm, 0, maxDrive);
        return y > 0 ? pwm : -pwm;
    }

    // --- keys -----------------------------------------------------------------------------

    public void Key(ManualKey key, bool down)
    {
        switch (key)
        {
            case ManualKey.Left:
                _keyLeft = down;
                if (down) _lastSteerKey = -1;
                break;
            case ManualKey.Right:
                _keyRight = down;
                if (down) _lastSteerKey = 1;
                break;
            case ManualKey.Forward:
                _keyForward = down;
                if (down) { _lastDriveKey = 1; _setPoint = 0; _wheelAccumulator = 0; }
                break;
            case ManualKey.Reverse:
                _keyReverse = down;
                if (down) { _lastDriveKey = -1; _setPoint = 0; _wheelAccumulator = 0; }
                break;
        }
    }

    private int KeySteer => _keyLeft && _keyRight ? _lastSteerKey : _keyLeft ? -1 : _keyRight ? 1 : 0;
    private int KeyDrive => (_keyForward && _keyReverse ? _lastDriveKey : _keyForward ? 1 : _keyReverse ? -1 : 0) * _maxDrive;

    // --- scrolling --------------------------------------------------------------------------

    /// <summary>Vertical scroll. <paramref name="delta"/> in Windows wheel units (120 per notch), positive = away from the user = faster forward.</summary>
    public void Wheel(int delta, long nowMs)
    {
        if (_dragging || delta == 0) return;
        var direction = Math.Sign(delta);
        if (_setPoint == 0 && direction == _detentDirection && nowMs < _detentUntilMs)
        {
            // Resting at zero: the same swipe does not carry on into the other direction.
            // Scrolling on keeps it there; only a pause, then a new scroll, goes on.
            _wheelAccumulator = 0;
            _detentUntilMs = nowMs + ZeroDetentMs;
            return;
        }
        if (Math.Sign(_wheelAccumulator) == -direction) _wheelAccumulator = 0;
        // One notch (WheelNotch delta units) moves the set-point by WheelStep: count in
        // delta × WheelStep so that small touchpad deltas add up exactly to a notch.
        _wheelAccumulator += (long)delta * WheelStep;
        var steps = (int)(_wheelAccumulator / WheelNotch);   // towards zero
        if (steps == 0) return;
        _wheelAccumulator -= (long)steps * WheelNotch;
        var next = _setPoint + steps;
        if ((_setPoint > 0 && next < 0) || (_setPoint < 0 && next > 0))
        {
            next = 0;
            _wheelAccumulator = 0;
            _detentDirection = direction;
            _detentUntilMs = nowMs + ZeroDetentMs;
        }
        else if (next == 0)
        {
            _detentDirection = direction;
            _detentUntilMs = nowMs + ZeroDetentMs;
        }
        _setPoint = Math.Clamp(next, -_maxDrive, _maxDrive);
    }

    /// <summary>Horizontal scroll. <paramref name="delta"/> positive = to the right (before <see cref="InvertHorizontal"/>).</summary>
    public void HorizontalWheel(int delta, long nowMs)
    {
        if (delta == 0) return;
        var d = InvertHorizontal ? -delta : delta;
        if (Math.Sign(_swipeAccumulator) == -Math.Sign(d)) _swipeAccumulator = 0;
        _swipeAccumulator += d;
        if (Math.Abs(_swipeAccumulator) >= SwipeThreshold)
        {
            _swipeSteer = Math.Sign(_swipeAccumulator);
            _swipeAccumulator = Math.Sign(_swipeAccumulator) * SwipeThreshold;   // a reversal needs a full threshold again
        }
        _lastSwipeMs = nowMs;
    }

    // --- stop and the command ---------------------------------------------------------------

    /// <summary>Everything to zero: pad released, keys up, set-point 0, steering centred.</summary>
    public void Stop()
    {
        PadUp();
        _keyLeft = _keyRight = _keyForward = _keyReverse = false;
        _setPoint = 0;
        _wheelAccumulator = 0;
        _detentDirection = 0;
        _swipeSteer = 0;
        _swipeAccumulator = 0;
    }

    /// <summary>The command to send now (also ends a horizontal scroll that has stopped).</summary>
    public ManualCommand Command(long nowMs)
    {
        if (_swipeSteer != 0 || _swipeAccumulator != 0)
        {
            if (nowMs - _lastSwipeMs >= SwipeReleaseMs)
            {
                _swipeSteer = 0;
                _swipeAccumulator = 0;
            }
        }
        if (_dragging) return ManualCommand.FromSigned(_dragSteer, _dragDrive, ManualSource.Pad);
        var keySteer = KeySteer;
        var keyDrive = KeyDrive;
        var steer = keySteer != 0 ? keySteer : _swipeSteer;
        var drive = keyDrive != 0 ? keyDrive : _setPoint;
        var source = keySteer != 0 || keyDrive != 0 ? ManualSource.Keys
                   : _swipeSteer != 0 || _setPoint != 0 ? ManualSource.Scroll
                   : ManualSource.None;
        return ManualCommand.FromSigned(steer, drive, source);
    }
}
