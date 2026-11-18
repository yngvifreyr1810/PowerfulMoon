#pragma once

#include <stdbool.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    float baseline[3];  // slowly follows the controller's resting acceleration
    bool ready;
} MotionFilter;

static inline float motion_filter_gain(const float v[3], float threshold, float ramp) {
    float magnitude = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    float t = fmaxf(0.0f, fminf(1.0f, (magnitude - threshold) / ramp));
    return t * t * (3.0f - 2.0f * t);
}

// Runs once per 5 ms history sample. Leave the pose and calibration alone.
static inline void motion_filter_sample(MotionFilter *filter, float sample[18], int level, bool live) {
    if (!live || level == 0) {
        filter->ready = false;
        return;
    }
    if (!filter->ready) {
        memcpy(filter->baseline, sample, sizeof filter->baseline);
        filter->ready = true;
    }
    float delta[3];
    for (int i = 0; i < 3; i++) delta[i] = sample[i] - filter->baseline[i];
    // The strong profile is for deliberate flicks. Logged preparation reached
    // nearly one turn/s, followed by much stronger motion in the other direction.
    float accel_gain = level == 2 ? motion_filter_gain(delta, 0.75f, 0.55f)
                                 : motion_filter_gain(delta, 0.18f, 0.18f);
    float gyro_gain = level == 2 ? motion_filter_gain(sample + 3, 1.05f, 0.45f)
                                : motion_filter_gain(sample + 3, 0.12f, 0.12f);
    for (int i = 0; i < 3; i++) {
        filter->baseline[i] += 0.06f * delta[i];
        sample[i] += (accel_gain - 1.0f) * (1.0f - 0.06f) * delta[i];
        sample[3 + i] *= gyro_gain;
    }
}

static inline void motion_gravity(const float direction[9], float down[3]) {
    // The ABI stores each controller basis vector in world space as a row.
    // Its world-Z component gives the corresponding component of gravity.
    for (int i = 0; i < 3; i++) down[i] = -direction[3 * i + 2];
}

static inline void motion_rotate_direction(float direction[9], const float rotation[3]) {
    float angle = sqrtf(rotation[0] * rotation[0] + rotation[1] * rotation[1] + rotation[2] * rotation[2]);
    if (angle < 1e-9f) return;
    float x = rotation[0] / angle, y = rotation[1] / angle, z = rotation[2] / angle;
    float c = cosf(angle), s = sinf(angle), t = 1 - c;
    float step[9] = {t*x*x+c, t*x*y-s*z, t*x*z+s*y,
                     t*y*x+s*z, t*y*y+c, t*y*z-s*x,
                     t*z*x-s*y, t*z*y+s*x, t*z*z+c};
    // The gyro is expressed in controller axes. Left-multiply the stored
    // basis by the transpose of the incremental rotation.
    float next[9];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            next[i * 3 + j] = step[i] * direction[j] +
                              step[i + 3] * direction[3 + j] +
                              step[i + 6] * direction[6 + j];
    for (int i = 0; i < 3; i++) {
        float *row = next + i * 3;
        for (int j = 0; j < i; j++) {
            const float *basis = next + j * 3;
            float dot = row[0] * basis[0] + row[1] * basis[1] + row[2] * basis[2];
            for (int k = 0; k < 3; k++) row[k] -= dot * basis[k];
        }
        float length = sqrtf(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]);
        if (length > 1e-6f)
            for (int k = 0; k < 3; k++) row[k] /= length;
    }
    memcpy(direction, next, sizeof next);
}

typedef struct {
    float bias[3], last_accel[3];
    uint64_t last_ns;
} MotionBias;

// Learn only small, stationary gyro offsets during a settling window. Real
// turns/shakes do not update the calibration; their values pass through.
static inline void motion_bias_sample(MotionBias *b, const float accel[3], float gyro[3], bool settling, uint64_t now) {
    float gravity = 0, rotation = 0, change = 0;
    for (int i = 0; i < 3; i++) {
        gravity += accel[i] * accel[i];
        rotation += gyro[i] * gyro[i];
        float d = accel[i] - b->last_accel[i];
        change += d * d;
    }
    if (settling && b->last_ns && gravity > 0.8f * 0.8f && gravity < 1.2f * 1.2f &&
        rotation < 0.02f * 0.02f && change < 0.02f * 0.02f) {
        float dt = now > b->last_ns && now - b->last_ns < 50000000ull ? (float)(now - b->last_ns) / 1e9f : 0.005f;
        float alpha = dt / (0.15f + dt);
        for (int i = 0; i < 3; i++) b->bias[i] += alpha * (gyro[i] - b->bias[i]);
    }
    memcpy(b->last_accel, accel, sizeof b->last_accel);
    b->last_ns = now;
    for (int i = 0; i < 3; i++) gyro[i] -= b->bias[i];
}

typedef struct {
    float last[18], resume_accel[3];
    bool blocked;
    uint64_t resume_ns, settle_until_ns;
} MotionGate;

static inline void motion_gate_init(MotionGate *g) {
    memset(g, 0, sizeof *g);
    g->last[2] = -1;
    g->last[9] = g->last[13] = g->last[17] = 1;
}

static inline void motion_gate_settle(MotionGate *g, uint64_t now) {
    g->settle_until_ns = now + 1000000000ull;
    g->blocked = true;
}

// Keep the reported gravity and orientation continuous at menu transitions.
// A synthetic identity pose is a physical jerk to the game's shake detector.
static inline void motion_gate_sample(MotionGate *g, float sample[18], bool blocked, uint64_t now) {
    blocked = blocked || now < g->settle_until_ns;
    if (blocked) {
        memcpy(sample, g->last, sizeof g->last);
        memset(sample + 3, 0, 3 * sizeof(float));
        g->blocked = true;
    } else {
        if (g->blocked) {
            memcpy(g->resume_accel, g->last, sizeof g->resume_accel);
            g->resume_ns = now;
            g->blocked = false;
        }
        if (g->resume_ns && now - g->resume_ns < 250000000ull) {
            float blend = (float)(now - g->resume_ns) / 250000000.0f;
            for (int i = 0; i < 3; i++) sample[i] = g->resume_accel[i] + blend * (sample[i] - g->resume_accel[i]);
            memset(sample + 3, 0, 3 * sizeof(float));
        }
    }
    memcpy(g->last, sample, sizeof g->last);
}
