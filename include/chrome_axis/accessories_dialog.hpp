#pragma once

#define IDD_ACCESSORIES 201
#define IDC_ACCESSORY_DEVICE 2101
#define IDC_ACCESSORY_REFRESH 2102
#define IDC_ACCESSORY_TYPE 2103
#define IDC_ACCESSORY_CAPTURE 2104
#define IDC_ACCESSORY_STATUS 2105
#define IDC_ACCESSORY_INPUT 2106
#define IDC_ACCESSORY_KEY 2107
#define IDC_ACCESSORY_ADD 2108
#define IDC_ACCESSORY_LIST 2109
#define IDC_ACCESSORY_REMOVE 2110
#define IDC_ACCESSORY_SHIFTER 2111
#define IDC_ACCESSORY_TYPE_LABEL 2112
#define IDC_ACCESSORY_INPUT_LABEL 2113
#define IDC_ACCESSORY_EDIT 2114
#define IDD_SHIFTER 202
#define IDC_SHIFTER_DEVICE 2201
#define IDC_SHIFTER_MODE 2202
#define IDC_SHIFTER_SIGNAL 2203
#define IDC_SHIFTER_START 2204
#define IDC_SHIFTER_STATUS 2205
#define IDC_SHIFTER_STEP 2206
#define IDC_SHIFTER_KEY 2207
#define IDC_SHIFTER_ADVANCE 2208
#define IDC_SHIFTER_SKIP 2209
#define IDC_SHIFTER_LIST 2210
#define IDC_SHIFTER_MODE_LABEL 2211
#define IDC_SHIFTER_SIGNAL_LABEL 2212
#define IDC_SHIFTER_EDIT 2213
#define IDC_MODAL_FORM 2301
#define IDC_MODAL_RESULTS 2302
#define IDC_MODAL_LIST_HEADING 2303
#define IDC_MODAL_INTRO 2304
#define IDC_MODAL_DEVICE_LABEL 2305
#define IDC_MODAL_KEY_LABEL 2306

#ifndef RC_INVOKED
#include "chrome_axis/accessories.hpp"

#include <filesystem>

namespace chrome_axis {

bool editAccessories(HWND owner, DirectInput& input, const std::filesystem::path& profileDirectory);
bool editShifter(HWND owner, DirectInput& input, AccessoryProfile& bindings, std::string_view preferredDevice);
std::wstring accessoryKeyLabel(unsigned int key);
std::wstring accessoryInputLabel(const AccessoryBinding& binding);
enum class BindingKeyEdit { unchanged, changed, occupied };
BindingKeyEdit chooseBindingKey(HWND owner, AccessoryProfile& bindings, std::size_t row);

} // namespace chrome_axis
#endif
