#ifndef PETARI_CAMERA_INPUT_H
#define PETARI_CAMERA_INPUT_H
/* Odyssey-style orbit camera (docs/dev/ODYSSEY_CAMERA.md): what the input layer
 * hands the game's camera each frame. Plain C, so SDK-side game code can include
 * it. All angles in degrees. */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PetariCameraInput {
    int enabled;      /* the OdysseyCamera mod is on */
    float stickX;     /* right stick now, -1..1 (x right, y up), dead zone applied */
    float stickY;
    float mouseYaw;   /* mouse drag since the last call (middle button held), degrees at speed 3 */
    float mousePitch;
    float yawHold;    /* -1/0/+1 while orbit left/right keys are held (J/L, Q/E, arrows) */
    float pitchHold;  /* -1/0/+1 while pitch keys are held (I = +1 camera rises, K = -1) */
    float zoomSteps;  /* wheel notches and Z/X presses since the last call (+ = out) */
    float zoomHold;   /* -1/0/+1 while Z/X are held (+ = out) */
    int recenter;     /* C pressed since the last call */
    int speed;        /* 1..5 */
    int invertX;
    int invertY;
} PetariCameraInput;

/* Game thread, once per frame: the input since the last call. */
void petari_camera_take_input(PetariCameraInput* out);

#ifdef __cplusplus
}
#endif

#endif
