#pragma once

// Compile the repository-owned FNK0104N driver under a distinct class name.
// Arduino IDE otherwise resolves ST77922.h from the globally installed
// TFT_eSPI library, bypassing this sketch's boot and landscape-transfer fixes.
#define ST77922 DollST77922
#include "../../libraries/TFT_eSPI/ST77922.h"
#undef ST77922
