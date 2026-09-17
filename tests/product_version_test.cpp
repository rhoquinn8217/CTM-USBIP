// The product's version: the string everything shows, and the numbers the exe's
// version resource is stamped with, must say the same thing.
//
// ⓘ A mismatch would not fail anywhere else. The window title and --version
// read the string, Explorer's Details tab reads the numbers, and each would look
// right on its own.

#include "harness.h"

#include <string>

#include "ctm/product.h"

using namespace ctmtest;

int run_product_version_tests()
{
    section("product version: the string matches the numbers");
    {
        const std::string fromNumbers = std::to_string(PRODUCT_VERSION_MAJOR) + "." +
                                        std::to_string(PRODUCT_VERSION_MINOR) + "." +
                                        std::to_string(PRODUCT_VERSION_PATCH);
        CTM_CHECK(fromNumbers == PRODUCT_VERSION);
    }

    section("product version: the name is ours, not the relay's");
    {
        CTM_CHECK(std::string(PRODUCT_NAME) == "DS5-USBIP");
        // ⓘ 0.x until a stranger can use it unaided (include/ctm/product.h).
        CTM_CHECK(PRODUCT_VERSION_MAJOR == 0);
    }
    return 0;
}
