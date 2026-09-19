// Same-device tests: which bridge sessions are parts of ONE device, and so
// share one nickname. ⛔ Not the same question as same_controller_test.cpp,
// which is about RETIRING a session -- see the header of same_device.inl.

#include "harness.h"

#include <string>

#include "app/same_device.inl"

using namespace ctmtest;

namespace {

// The three multi-part devices measured on 2026-09-19, with the ids the TV
// reported for each and the product string that actually reached the listener.
const uint16_t kRazerV = 0x1532, kRazerP = 0x0094;
const uint16_t kRongV = 0x25a7, kRongP = 0xfa17;

bool same(uint16_t v, uint16_t p, const char *serial, const char *product,
          uint16_t ov, uint16_t op, const char *otherSerial, const char *otherProduct)
{
    return same_device::matches(v, p, serial, product, ov, op, otherSerial, otherProduct);
}

} // namespace

int run_same_device_tests()
{
    section("same device: the two halves of one dongle share a nickname");
    {
        // ⛔ THE FAULT ITSELF. These two came up as "Bean" and "Luna".
        CTM_CHECK(same(kRongV, kRongP, "", "RONGYUAN 2.4G Wireless Device",
                       kRongV, kRongP, "", "RONGYUAN 2.4G Wireless Device"));
        // And these two as "Kraken" and "Rift", on the overnight run.
        CTM_CHECK(same(0x043e, 0x9a39, "", "KMA2 LG RF dongle A2",
                       0x043e, 0x9a39, "", "KMA2 LG RF dongle A2"));
        // The Razer's mouse and keyboard halves, "Strobe" and "Falcon".
        CTM_CHECK(same(kRazerV, kRazerP, "", "Razer Orochi V2",
                       kRazerV, kRazerP, "", "Razer Orochi V2"));
    }

    section("same device: two different devices never share one");
    {
        // The Razer and the RONGYUAN were bridged together and must stay apart.
        CTM_CHECK(!same(kRazerV, kRazerP, "", "Razer Orochi V2",
                        kRongV, kRongP, "", "RONGYUAN 2.4G Wireless Device"));
        // ⚠️ THE IDS ARE THE GUARD: same vendor, same name up to a suffix, and
        // still two devices because the product id differs.
        CTM_CHECK(!same(kRazerV, 0x0094, "", "Razer Orochi V2",
                        kRazerV, 0x00a5, "", "Razer Orochi V2 Pro"));
        // Two pads that answer to one name are not one pad.
        CTM_CHECK(!same(0x054c, 0x0ce6, "7c:66:ef:82:10:ed", "DualSense Wireless Controller",
                        0x054c, 0x0ce6, "a0:fa:9c:ef:9b:30", "DualSense Wireless Controller"));
    }

    section("same device: a part named for its role still matches");
    {
        // ⓘ The fallback. The listener receives the USB product string, so
        // both halves send the same one today -- this is the TV's own naming
        // ("iFEKER (mouse), iFEKER consumer control"), covered in case a
        // part's own name ever crosses.
        CTM_CHECK(same(kRazerV, kRazerP, "", "Razer Orochi V2",
                       kRazerV, kRazerP, "", "Razer Orochi V2 Consumer Control"));
        CTM_CHECK(same(kRongV, kRongP, "", "RONGYUAN 2.4G Wireless Device Keyboard",
                       kRongV, kRongP, "", "RONGYUAN 2.4G Wireless Device"));
        // ⚠️ And the space that makes it a suffix rather than a coincidence.
        CTM_CHECK(!same(kRazerV, kRazerP, "", "Orochi",
                        kRazerV, kRazerP, "", "Orochimaru"));
    }

    section("same device: one serial across every interface");
    {
        // T-182: a GameSir's pad and its keyboard both carry 3286967D. They
        // ARE one device -- this is exactly what same_controller.inl must not
        // conclude, and what this must.
        CTM_CHECK(same(0x3537, 0x1014, "3286967D", "GameSir-G8+",
                       0x3537, 0x1014, "3286967D", "GameSir-G8+"));
        // A serial against no serial makes no claim either way, so it fails
        // closed: two nicknames, which is visible and harmless.
        CTM_CHECK(!same(0x3537, 0x1014, "3286967D", "GameSir-G8+",
                        0x3537, 0x1014, "", "GameSir-G8+"));
    }

    section("same device: no name is no identity");
    {
        // ⛔ Every pre-HELLO session has an empty product AND, because
        // BackendCaps defaults to a DualSense, the same ids. Matching on the
        // ids alone would join all of them.
        CTM_CHECK(!same(0x054c, 0x0ce6, "", "", 0x054c, 0x0ce6, "", ""));
        CTM_CHECK(!same(kRongV, kRongP, "", "", kRongV, kRongP, "",
                        "RONGYUAN 2.4G Wireless Device"));
    }
    return 0;
}
