#include "JSystem/JMath/JMath.hpp"

extern "C" void petari_probe_matrix(unsigned frame, float matrix[3][4]) {
    Quaternion rotation;
    JMAEulerToQuat(0, 0, static_cast<s16>(frame * 128), &rotation);
    PSMTXQuat(matrix, &rotation);
}
