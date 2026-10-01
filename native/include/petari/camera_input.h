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

/* Whether the orbit currently drives the camera (game side; for test drivers). */
int petari_camera_orbit_active(void);

/* Photo mode (docs/dev/ODYSSEY_CAMERA.md "Photo mode"): the game frozen, a free
 * camera. Distances in world units per frame at normal speed, angles in degrees. */
typedef struct PetariPhotoInput {
    int enabled;        /* the PhotoMode setting is on */
    int toggle;         /* PhotoMode pressed since the last call (enter or leave) */
    int leave;          /* a leave-only input (Plus/Escape) pressed since the last call */
    int shots;          /* PhotoShot presses since the last call */
    float moveRight;    /* -1..1: strafe (A/D, left stick x) */
    float moveForward;  /* -1..1: along the view (W/S, left stick y) */
    float moveUp;       /* -1..1: along the camera's up (Space up, Shift down) */
    float yaw;          /* look, degrees since the last call (+ = turn right) */
    float pitch;        /* + = look up */
    float fovSteps;     /* + = wider */
    int fast;           /* speed modifier held (Tab) */
    int slow;           /* slow modifier held (Alt / Walk) */
} PetariPhotoInput;

/* Game thread, once per frame: the photo input since the last call. */
void petari_photo_take_input(PetariPhotoInput* out);
/* Game thread: photo mode started (1), or ended or refused (0; a toggle the game
 * cannot take now must be answered with 0). While a toggle waits and while active
 * the host sends the game no input (everything held is released on entry). */
void petari_photo_set_active(int active);

#ifdef __cplusplus
}
#endif

#endif
