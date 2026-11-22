/*
 * Copyright 2017-2022 GoPro Inc.
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "libnopeutil/crc32.h"
#include "libnopeutil/memory.h"
#include "libnopeutil/string.h"
#include "libnopeutil/utils.h"

static void test_numbered_line(uint32_t crc, const char *s)
{
    char *p = npriv_numbered_lines(s);
    npriv_assert(p);
    npriv_assert(npriv_crc32(p) == crc);
    npriv_freep(&p);
}

int main(void)
{
    npriv_assert(npriv_crc32("") == 0);
    npriv_assert(npriv_crc32("Hello world !@#$%^&*()_+") == 0xDCEB8676);

    char buf[256];
    for (size_t i = 0; i <= 0xff; i++)
        buf[i] = (char)(0xff - i);
    npriv_assert(npriv_crc32(buf) == 0x5473AA4D);

#define X "x\n"
#define S "foo\nbar\nhello\nworld\nbla\nxxx\nyyy\n"
    test_numbered_line(0xbf0d2015, S S S S S S S S);
    test_numbered_line(0x05a06081, "\n");
    test_numbered_line(0xd5331289, "foo\nbar");
    test_numbered_line(0x00000000, "");
    test_numbered_line(0x50d91f83, X X X X X X X X X);
    test_numbered_line(0x0eadb173, X X X X X X X X X X);
    test_numbered_line(0xd5af067e, "#version 450\n#line 1 2\nfoo\nbar\n#line 10\nbaz\n");
    return 0;
}
