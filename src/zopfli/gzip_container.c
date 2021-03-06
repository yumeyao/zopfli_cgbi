/*
Copyright 2013 Google Inc. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

Author: lode.vandevenne@gmail.com (Lode Vandevenne)
Author: jyrki.alakuijala@gmail.com (Jyrki Alakuijala)
*/

#include "gzip_container.h"
#include "util.h"

#include <stdio.h>

#include "crc.h"
#include "deflate.h"

/*
Compresses the data according to the gzip specification.
*/
void ZopfliGzipCompress(const ZopfliOptions* options,
                        const unsigned char* in, size_t insize,
                        unsigned char** out, size_t* outsize) {
  unsigned long crcvalue = lodepng_crc32(in, insize);
  unsigned char bp = 0;

  ZOPFLI_APPEND_DATA(31, out, outsize);  /* ID1 */
  ZOPFLI_APPEND_DATA(139, out, outsize);  /* ID2 */
  ZOPFLI_APPEND_DATA(8, out, outsize);  /* CM */
  ZOPFLI_APPEND_DATA(0, out, outsize);  /* FLG */
  /* MTIME */
  ZOPFLI_APPEND_DATA(0, out, outsize);
  ZOPFLI_APPEND_DATA(0, out, outsize);
  ZOPFLI_APPEND_DATA(0, out, outsize);
  ZOPFLI_APPEND_DATA(0, out, outsize);

  ZOPFLI_APPEND_DATA(2, out, outsize);  /* XFL, 2 indicates best compression. */
  ZOPFLI_APPEND_DATA(3, out, outsize);  /* OS follows Unix conventions. */

  ZopfliDeflate(options, 2 /* Dynamic block */, 1,
                in, insize, &bp, out, outsize);

  /* CRC */
  ZOPFLI_APPEND_DATA(crcvalue % 256, out, outsize);
  ZOPFLI_APPEND_DATA((crcvalue >> 8) % 256, out, outsize);
  ZOPFLI_APPEND_DATA((crcvalue >> 16) % 256, out, outsize);
  ZOPFLI_APPEND_DATA((crcvalue >> 24) % 256, out, outsize);

  /* ISIZE */
  ZOPFLI_APPEND_DATA(insize % 256, out, outsize);
  ZOPFLI_APPEND_DATA((insize >> 8) % 256, out, outsize);
  ZOPFLI_APPEND_DATA((insize >> 16) % 256, out, outsize);
  ZOPFLI_APPEND_DATA((insize >> 24) % 256, out, outsize);

  if (options->verbose) {
    ZopfliPrintSizeVerbose(insize, *outsize, "Gzip");
  }
}
