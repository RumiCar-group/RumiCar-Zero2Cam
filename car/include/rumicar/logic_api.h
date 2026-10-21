/*
 * RumiCar-Zero2Cam — driving-logic plug-in interface (C ABI).
 *
 * A driving logic is a shared library (.so) that exports one symbol:
 *
 *     const rumi_logic_api* rumi_logic_get_api(void);
 *
 * The core process (rumicar-core) loads the library with dlopen(), checks
 * abi_version, creates one instance with the active pack directory, runs
 * self_test(), and then calls step() once per sensor frame.  The logic is a
 * pure computation: it must not touch hardware, files (other than reading its
 * pack directory in create()), the network, or threads.
 *
 * This header is the single source of truth for the contract between the
 * core and every logic plug-in.  Changing any struct layout or semantics
 * requires bumping RUMI_LOGIC_ABI_VERSION.
 *
 * ABI history: 1 = sensors + camera image in, steer/drive/state/flags out.
 *              2 = adds the aux output in rumi_command and aux_name() (docs 3.6).
 */
#ifndef RUMICAR_LOGIC_API_H
#define RUMICAR_LOGIC_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RUMI_LOGIC_ABI_VERSION 2u

/* Name of the exported entry point. */
#define RUMI_LOGIC_ENTRY_SYMBOL "rumi_logic_get_api"

/* Sensor indices in rumi_sensor_frame::tof_mm. */
#define RUMI_TOF_LEFT   0
#define RUMI_TOF_CENTER 1
#define RUMI_TOF_RIGHT  2
#define RUMI_TOF_COUNT  3

/* Time-of-flight reading conventions (millimetres).
 *   >= 0                : measured distance
 *   RUMI_TOF_NO_TARGET  : sensor reports no target within its range
 *   RUMI_TOF_INVALID    : no reading (communication error, timeout)
 */
#define RUMI_TOF_NO_TARGET 8190
#define RUMI_TOF_INVALID   (-1)

/* Steering values in rumi_command::steer. */
#define RUMI_STEER_LEFT   (-1)
#define RUMI_STEER_CENTER 0
#define RUMI_STEER_RIGHT  1

/* Drive range in rumi_command::drive_permille (per mille of full duty). */
#define RUMI_DRIVE_MAX 1000
#define RUMI_DRIVE_MIN (-1000)

/* Bits in rumi_command::flags. */
#define RUMI_CMD_FLAG_BRAKE   0x1u /* hold the drive motor braked (both inputs high) */
#define RUMI_CMD_FLAG_FAULT   0x2u /* logic has detected a sensor fault; core must keep motors stopped */

/*
 * One sensor frame handed to the logic.  `size` is sizeof(rumi_sensor_frame)
 * as compiled into the core, so a logic built against a newer minor revision
 * can detect missing trailing fields.
 */
typedef struct rumi_sensor_frame {
    uint32_t       size;
    uint32_t       seq;            /* frame counter, increments by one per frame */
    uint64_t       t_us;           /* monotonic timestamp, microseconds */
    int32_t        tof_mm[RUMI_TOF_COUNT];
    /* Camera image.  NULL when no camera frame accompanies this sensor frame.
     * Pixel format is RGB24, row-major, `image_stride` bytes per row. */
    const uint8_t* image;
    uint32_t       image_width;
    uint32_t       image_height;
    uint32_t       image_stride;
} rumi_sensor_frame;

/* Size of rumi_command::aux. */
#define RUMI_AUX_BYTES 48u

/* Command produced by the logic for this frame. */
typedef struct rumi_command {
    uint32_t size;
    int8_t   steer;          /* RUMI_STEER_* */
    int16_t  drive_permille; /* RUMI_DRIVE_MIN .. RUMI_DRIVE_MAX, negative = reverse */
    uint8_t  state;          /* logic-defined state id, reported in telemetry */
    uint32_t flags;          /* RUMI_CMD_FLAG_* */
    /* Auxiliary output (ABI 2): logic-defined bytes the core forwards unchanged
     * in telemetry (base64) for display on the PC, e.g. detector scores and
     * positions.  aux_format identifies the layout (0 = none); aux_len bytes of
     * aux are valid (0 .. RUMI_AUX_BYTES).  Never used for driving. */
    uint16_t aux_format;
    uint8_t  aux_len;
    uint8_t  aux[RUMI_AUX_BYTES];
} rumi_command;

typedef struct rumi_logic_api {
    uint32_t    abi_version; /* must equal RUMI_LOGIC_ABI_VERSION */
    const char* name;        /* logic name, e.g. "tof_drive"; packs declare which logic they are for */
    const char* version;     /* semantic version of this logic build */

    /* Create an instance.  `pack_dir` is the directory of the active pack
     * (contains manifest.json).  On failure returns NULL and writes a message
     * into err (NUL-terminated, at most err_len bytes). */
    void* (*create)(const char* pack_dir, char* err, size_t err_len);

    void (*destroy)(void* instance);

    /* Run internal consistency checks with the loaded parameters.
     * Returns 0 on success; otherwise writes a message into err. */
    int (*self_test)(void* instance, char* err, size_t err_len);

    /* Compute the command for one frame.  Returns 0 on success.  A non-zero
     * return means the logic could not produce a command; the core then stops
     * the motors for this frame. */
    int (*step)(void* instance, const rumi_sensor_frame* frame, rumi_command* out);

    /* Human-readable name for a state id (for telemetry/logs).  Never NULL. */
    const char* (*state_name)(void* instance, uint8_t state);

    /* ABI 2: name of the index-th item described by the aux output (e.g. the
     * detector whose score is in slot `index`), or NULL beyond the last one.
     * The core lists them in get_status (aux_names) so the PC can label what
     * it draws.  May return NULL for every index when aux is not used. */
    const char* (*aux_name)(void* instance, uint8_t index);
} rumi_logic_api;

typedef const rumi_logic_api* (*rumi_logic_get_api_fn)(void);

#ifdef __cplusplus
}
#endif

#endif /* RUMICAR_LOGIC_API_H */
