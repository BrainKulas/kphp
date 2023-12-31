// Compiler for PHP (aka KPHP)
// Copyright (c) 2021 LLC «V Kontakte»
// Distributed under the GPL v3 License, see LICENSE.notice.txt

/* Autogenerated from net.tl and left only used constants */
#pragma once

#include <cstdint>

#define TL_STATSHOUSE_ADD_METRICS_BATCH 0x56580239U
#define TL_STATSHOUSE_METRIC 0x3325d884U

namespace vk {
namespace tl {
namespace statshouse {

namespace add_metrics_batch_fields_mask {
constexpr static uint32_t ALL = 0x00000000;
} // namespace add_metrics_batch_fields_mask

namespace metric_fields_mask {
constexpr static uint32_t counter = 1U << 0U;
constexpr static uint32_t t = 1U << 5U;
constexpr static uint32_t value = 1U << 1U;
constexpr static uint32_t unique = 1U << 2U;
constexpr static uint32_t stop = 1U << 3U;
constexpr static uint32_t ALL = 0x0100002f;
} // namespace metric_fields_mask

} // namespace statshouse
} // namespace tl
} // namespace vk
