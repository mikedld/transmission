// This file Copyright © 2022-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import <Foundation/Foundation.h>

#ifdef __cplusplus
// clang-format off
#define TR_EXTERN_C_BEGIN \
    extern "C" \
    {
#define TR_EXTERN_C_END \
    }
// clang-format on
#else
#define TR_EXTERN_C_BEGIN
#define TR_EXTERN_C_END
#endif
