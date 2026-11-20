// Tests of what goes to the drive motor (docs 3.4):
//  - ManualOutputLimiter: the manual acceleration (a rising duty climbs at the
//    rate, a falling one, FREE and BRAKE follow at once, a reversal starts from 0);
//  - FrameOutputStage: the decision of a frame as the control loop makes it
//    (modes, the caps within max_drive_pwm, the steering strength, the cap of
//    the start kick);
//  - Drv8835's start kick on the real driver: its duty capped at the mode's
//    cap, and its end after kick_ms by the worker thread, whatever the loop
//    does meanwhile.  The PWM channels are directories laid out as the
//    kernel's sysfs PWM chip (pwmN/period, duty_cycle, enable), so the driver
//    writes exactly the files it writes on the car; the steering lines go to
//    a recording function.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>

#include <unistd.h>

#include "check.hpp"
#include "frame_output.hpp"
#include "hw/drv8835.hpp"
#include "manual_output.hpp"
#include "rumicar/logic_api.h"

using namespace rumicar;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

constexpr double kFrame = 0.05;   // the control loop's period

void test_manual_limiter() {
    ManualOutputLimiter m;
    // From rest at 287 per second: 14.35 a frame.
    CHECK(m.drive(RUMI_DRIVE_FORWARD, 153, 287, kFrame) == 14);
    CHECK(m.drive(RUMI_DRIVE_FORWARD, 153, 287, kFrame) == 28);
    int v = 0;
    for (int i = 0; i < 20; ++i) v = m.drive(RUMI_DRIVE_FORWARD, 153, 287, kFrame);
    CHECK(v == 153);   // reaches the duty asked for, never above
    // Down at once, then up again at the rate from there.
    CHECK(m.drive(RUMI_DRIVE_FORWARD, 60, 287, kFrame) == 60);
    CHECK(m.drive(RUMI_DRIVE_FORWARD, 153, 287, kFrame) == 74);
    // Reversing starts from rest.
    CHECK(m.drive(RUMI_DRIVE_REVERSE, 153, 287, kFrame) == 14);
    // Stopping is at once, and the next start is from rest.
    CHECK(m.drive(RUMI_DRIVE_BRAKE, 0, 287, kFrame) == 0);
    CHECK(m.drive(RUMI_DRIVE_REVERSE, 153, 287, kFrame) == 14);
    CHECK(m.drive(RUMI_DRIVE_FREE, 0, 287, kFrame) == 0);
    CHECK(m.drive(RUMI_DRIVE_FORWARD, 153, 287, kFrame) == 14);
    // A pause in the frames counts at most 0.2 s.
    CHECK(m.drive(RUMI_DRIVE_FORWARD, 255, 287, 5.0) == 71);   // 14.35 + 57.4
    // 0 = no limit: the duty asked for at once.
    m.idle();
    CHECK(m.drive(RUMI_DRIVE_FORWARD, 200, 0, kFrame) == 200);
    // idle() (another mode, an e-stop): the next run starts from rest.
    m.idle();
    CHECK(m.drive(RUMI_DRIVE_FORWARD, 200, 287, kFrame) == 14);
    // 0 to 255 at 287 per second takes 0.89 s: 18 frames of 50 ms.
    m.idle();
    int frames = 0;
    for (v = 0; v < 255 && frames < 100; ++frames) v = m.drive(RUMI_DRIVE_FORWARD, 255, 287, kFrame);
    CHECK_MSG(frames == 18, std::to_string(frames));
}

CarState::FrameInputs inputs(DriveMode mode) {
    CarState::FrameInputs in{};
    in.mode = mode;
    in.estop = false;
    in.manual_steer = RUMI_STEER_CENTER;
    in.manual_steer_pwm = 0;
    in.manual_drive_dir = RUMI_DRIVE_FREE;
    in.manual_drive_pwm = 0;
    in.manual_timed_out = false;
    in.auto_limit = 255;
    in.manual_max_pwm = 153;
    in.manual_accel_pwm_per_s = 287;
    return in;
}

void test_frame_output() {
    rumi_command cmd{};
    cmd.size = sizeof(cmd);
    cmd.steer = RUMI_STEER_LEFT;
    cmd.steer_pwm = 128;
    cmd.drive_dir = RUMI_DRIVE_FORWARD;
    cmd.drive_pwm = 230;

    {
        // Manual: the PC's steering with its strength, the manual cap within
        // max_drive_pwm, the acceleration, and the kick capped at the manual cap.
        FrameOutputStage st(287);
        auto in = inputs(DriveMode::Manual);
        in.manual_steer = RUMI_STEER_RIGHT;
        in.manual_steer_pwm = 90;
        in.manual_drive_dir = RUMI_DRIVE_FORWARD;
        in.manual_drive_pwm = 255;   // more than the cap
        in.manual_max_pwm = 120;
        FrameOutput o = st.step(in, 0, cmd, 255, kFrame);
        CHECK(o.steer == RUMI_STEER_RIGHT && o.steer_pwm == 90);
        CHECK(o.drive_dir == RUMI_DRIVE_FORWARD && o.drive_pwm == 14);
        CHECK(o.kick_cap == 120);
        for (int i = 0; i < 20; ++i) o = st.step(in, 0, cmd, 255, kFrame);
        CHECK(o.drive_pwm == 120);
        CHECK(st.step(in, 0, cmd, 100, kFrame).kick_cap == 100);   // max_drive_pwm below the manual cap
        in.manual_steer = RUMI_STEER_CENTER;
        in.manual_steer_pwm = 200;
        CHECK(st.step(in, 0, cmd, 255, kFrame).steer_pwm == 0);    // CENTER has no strength
        in.manual_steer = RUMI_STEER_LEFT;
        in.manual_steer_pwm = 255;
        CHECK(st.step(in, 0, cmd, 255, kFrame).steer_pwm == 255);  // the keys: full
        // Manual acceleration off: the duty at once (the earlier behaviour).
        FrameOutputStage st0(287);
        in.manual_accel_pwm_per_s = 0;
        CHECK(st0.step(in, 0, cmd, 255, kFrame).drive_pwm == 120);
        // The logic's command plays no part in manual mode, nor does its failure.
        CHECK(st0.step(in, -1, cmd, 255, kFrame).drive_pwm == 120);
    }
    {
        // An e-stop or another mode in between: manual starts from rest again.
        FrameOutputStage st(287);
        auto in = inputs(DriveMode::Manual);
        in.manual_drive_dir = RUMI_DRIVE_FORWARD;
        in.manual_drive_pwm = 153;
        for (int i = 0; i < 20; ++i) st.step(in, 0, cmd, 255, kFrame);
        CHECK(st.step(in, 0, cmd, 255, kFrame).drive_pwm == 153);
        auto stopped = in;
        stopped.estop = true;
        const FrameOutput o = st.step(stopped, 0, cmd, 255, kFrame);
        CHECK(o.drive_dir == RUMI_DRIVE_BRAKE && o.drive_pwm == 0 && o.kick_cap == 0 && o.steer == RUMI_STEER_CENTER);
        CHECK(st.step(in, 0, cmd, 255, kFrame).drive_pwm == 14);
        for (int i = 0; i < 20; ++i) st.step(in, 0, cmd, 255, kFrame);
        st.step(inputs(DriveMode::RecordOnly), 0, cmd, 255, kFrame);
        CHECK(st.step(in, 0, cmd, 255, kFrame).drive_pwm == 14);
    }
    {
        // Auto: the logic's command, the start ramp, the auto limit within
        // max_drive_pwm, and the kick capped at it.
        FrameOutputStage st(287);
        auto in = inputs(DriveMode::Auto);
        in.auto_limit = 180;
        FrameOutput o = st.step(in, 0, cmd, 255, kFrame);
        CHECK(o.steer == RUMI_STEER_LEFT && o.steer_pwm == 128);
        CHECK(o.drive_dir == RUMI_DRIVE_FORWARD && o.drive_pwm == 14 && o.kick_cap == 180);
        for (int i = 0; i < 20; ++i) o = st.step(in, 0, cmd, 255, kFrame);
        CHECK(o.drive_pwm == 180);
        CHECK(st.step(in, 0, cmd, 150, kFrame).kick_cap == 150);
        // A logic failure brakes; a FAULT flag brakes the drive and keeps the steering.
        o = st.step(in, -1, cmd, 255, kFrame);
        CHECK(o.drive_dir == RUMI_DRIVE_BRAKE && o.steer == RUMI_STEER_CENTER && o.kick_cap == 0);
        rumi_command fault = cmd;
        fault.flags = RUMI_CMD_FLAG_FAULT;
        o = st.step(in, 0, fault, 255, kFrame);
        CHECK(o.drive_dir == RUMI_DRIVE_BRAKE && o.drive_pwm == 0 && o.steer == RUMI_STEER_LEFT && o.kick_cap == 0);
        // Record only: braked.
        o = st.step(inputs(DriveMode::RecordOnly), 0, cmd, 255, kFrame);
        CHECK(o.drive_dir == RUMI_DRIVE_BRAKE && o.drive_pwm == 0 && o.kick_cap == 0);
    }
}

// --- the start kick on the real driver ---------------------------------------------

constexpr int kPwmHz = 960;
constexpr long kPeriodNs = 1'000'000'000L / kPwmHz;

// A directory laid out as /sys/class/pwm/pwmchip0 with pwm0 and pwm1 exported.
fs::path make_pwm_chip() {
    const fs::path chip = fs::temp_directory_path() / ("rumicar-pwmchip-" + std::to_string(::getpid()));
    fs::remove_all(chip);
    for (const char* ch : {"pwm0", "pwm1"}) {
        fs::create_directories(chip / ch);
        for (const char* f : {"period", "duty_cycle", "enable"}) std::ofstream(chip / ch / f) << 0;
    }
    std::ofstream(chip / "export") << "";
    return chip;
}

// The duty (0..255) a channel is set to, from its duty_cycle file; nullopt while the file is being rewritten.
std::optional<int> duty(const fs::path& chip, int channel) {
    std::ifstream in(chip / ("pwm" + std::to_string(channel)) / "duty_cycle");
    long ns = -1;
    if (!(in >> ns) || ns < 0) return std::nullopt;
    return static_cast<int>((ns * 255 + kPeriodNs / 2) / kPeriodNs);
}

// The drive duty seen on BIN2 when going forward (slow decay: IN2 = 255 - s).
std::optional<int> forward_duty(const fs::path& chip) {
    const auto d = duty(chip, 1);
    if (!d) return std::nullopt;
    return 255 - *d;
}

// Waits until the forward duty is `want`; returns the time it took, or -1 ms after `limit`.
std::chrono::microseconds wait_forward(const fs::path& chip, int want, Clock::time_point since, std::chrono::milliseconds limit) {
    while (Clock::now() - since < limit) {
        const auto d = forward_duty(chip);
        if (d && *d == want) return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - since);
        std::this_thread::sleep_for(100us);
    }
    return std::chrono::microseconds(-1000);
}

void test_kick() {
    const fs::path chip = make_pwm_chip();
    const auto no_steering = [](unsigned, bool) {};
    {
        hw::Drv8835 drv(no_steering, 500, chip.string(), kPwmHz, hw::Drv8835::Pins{0, 1}, hw::Drv8835::Kick{200, 20});
        CHECK(std::ifstream(chip / "pwm0" / "period").good());

        // A start from rest at 50: the kick (200) at once, then 50 after 20 ms,
        // ended by the worker (no further apply() in between, as when the loop
        // runs every 50 ms).
        auto t = Clock::now();
        drv.apply(RUMI_STEER_CENTER, 0, RUMI_DRIVE_FORWARD, 50, 255);
        CHECK(duty(chip, 0) == 255);          // IN1 high
        CHECK(forward_duty(chip) == 200);     // kicking
        auto end = wait_forward(chip, 50, t, 200ms);
        CHECK_MSG(end >= 19ms && end <= 30ms, std::to_string(end.count()) + " us");

        // Frames during the kick do not lengthen it, and the duty they ask for follows after it.
        drv.apply(RUMI_STEER_CENTER, 0, RUMI_DRIVE_BRAKE, 0, 0);
        t = Clock::now();
        drv.apply(RUMI_STEER_CENTER, 0, RUMI_DRIVE_FORWARD, 40, 255);
        for (int i = 0; i < 3; ++i) {
            std::this_thread::sleep_for(4ms);
            drv.apply(RUMI_STEER_CENTER, 0, RUMI_DRIVE_FORWARD, 60, 255);
            CHECK(forward_duty(chip) == 200);
        }
        end = wait_forward(chip, 60, t, 200ms);
        CHECK_MSG(end >= 19ms && end <= 30ms, std::to_string(end.count()) + " us");

        // The kick never exceeds the cap of the frame (the mode's cap).
        drv.apply(RUMI_STEER_CENTER, 0, RUMI_DRIVE_BRAKE, 0, 0);
        t = Clock::now();
        drv.apply(RUMI_STEER_CENTER, 0, RUMI_DRIVE_FORWARD, 30, 120);
        CHECK(forward_duty(chip) == 120);
        end = wait_forward(chip, 30, t, 200ms);
        CHECK_MSG(end >= 19ms && end <= 30ms, std::to_string(end.count()) + " us");
        // A cap at or below the duty: no kick to see.
        drv.apply(RUMI_STEER_CENTER, 0, RUMI_DRIVE_BRAKE, 0, 0);
        drv.apply(RUMI_STEER_CENTER, 0, RUMI_DRIVE_FORWARD, 100, 100);
        CHECK(forward_duty(chip) == 100);
        std::this_thread::sleep_for(30ms);
        CHECK(forward_duty(chip) == 100);

        // A reversal kicks again (on IN1, IN2 high).
        drv.apply(RUMI_STEER_CENTER, 0, RUMI_DRIVE_REVERSE, 50, 255);
        CHECK(duty(chip, 1) == 255 && duty(chip, 0) == 255 - 200);
        std::this_thread::sleep_for(40ms);
        CHECK(duty(chip, 0) == 255 - 50);

        // Stopping during a kick ends it at once, and nothing is written after.
        drv.apply(RUMI_STEER_CENTER, 0, RUMI_DRIVE_FREE, 0, 0);
        drv.apply(RUMI_STEER_CENTER, 0, RUMI_DRIVE_FORWARD, 50, 255);
        CHECK(forward_duty(chip) == 200);
        drv.apply(RUMI_STEER_CENTER, 0, RUMI_DRIVE_FREE, 0, 0);
        CHECK(duty(chip, 0) == 0 && duty(chip, 1) == 0);
        drv.apply(RUMI_STEER_CENTER, 0, RUMI_DRIVE_FORWARD, 50, 255);
        CHECK(forward_duty(chip) == 200);
        drv.stop();
        CHECK(duty(chip, 0) == 255 && duty(chip, 1) == 255);   // brake
        std::this_thread::sleep_for(40ms);
        CHECK(duty(chip, 0) == 255 && duty(chip, 1) == 255);   // the ended kick wrote nothing

        // The loop as it runs on the car: a frame every 50 ms with the manual
        // acceleration; the kick lasts 20 ms of the first frame only.
        FrameOutputStage st(287);
        auto in = inputs(DriveMode::Manual);
        in.manual_drive_dir = RUMI_DRIVE_FORWARD;
        in.manual_drive_pwm = 153;
        rumi_command cmd{};
        t = Clock::now();
        FrameOutput o = st.step(in, 0, cmd, 255, kFrame);
        drv.apply(o.steer, o.steer_pwm, o.drive_dir, o.drive_pwm, o.kick_cap);
        CHECK(o.drive_pwm == 14 && forward_duty(chip) == 153);   // the kick, capped at the manual cap 153
        end = wait_forward(chip, 14, t, 200ms);
        CHECK_MSG(end >= 19ms && end <= 30ms, std::to_string(end.count()) + " us");
        std::this_thread::sleep_until(t + 50ms);
        o = st.step(in, 0, cmd, 255, kFrame);
        drv.apply(o.steer, o.steer_pwm, o.drive_dir, o.drive_pwm, o.kick_cap);
        CHECK(o.drive_pwm == 28 && forward_duty(chip) == 28);
        std::printf("start kick worker: %s\n", drv.kick_realtime() ? "SCHED_FIFO" : "normal scheduling");
    }
    {
        // kick_ms 0 or kick_pwm 0 (core.json): no kick.
        hw::Drv8835 drv(no_steering, 500, chip.string(), kPwmHz, hw::Drv8835::Pins{0, 1}, hw::Drv8835::Kick{0, 20});
        drv.apply(RUMI_STEER_CENTER, 0, RUMI_DRIVE_FORWARD, 50, 255);
        CHECK(forward_duty(chip) == 50);
    }
    fs::remove_all(chip);
}

} // namespace

int main() {
    test_manual_limiter();
    test_frame_output();
    test_kick();
    return check::summary("test_drive_output");
}
