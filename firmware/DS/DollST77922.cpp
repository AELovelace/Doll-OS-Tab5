// Keep the implementation in sync with the vendored driver without duplicating
// its panel initialization table and QSPI transaction code.
#define ST77922 DollST77922
#include "../../libraries/TFT_eSPI/ST77922.cpp"
#undef ST77922
