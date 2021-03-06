/* Copyright (c) 2016, 2017, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef MYSQL_CHECKER_VER_2_0_H
#define MYSQL_CHECKER_VER_2_0_H

#include "checker.h"
#include "digest.h"
#include "my_inttypes.h"
#include "my_io.h"

namespace keyring {

class CheckerVer_2_0 : public Checker
{
public:
  CheckerVer_2_0() : Checker(keyring_file_version_2_0)
  {}
protected:
  bool is_file_size_correct(size_t file_size);
  bool file_seek_to_tag(File file);
  bool is_dgst_correct(File file, Digest *dgst);
};

}//namespace keyring

#endif //MYSQL_CHECKER_VER_2_0_H
