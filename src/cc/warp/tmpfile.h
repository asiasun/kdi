//---------------------------------------------------------- -*- Mode: C++ -*-
// Copyright (C) 2006 Josh Taylor (Kosmix Corporation)
// Created 2006-04-03
// 
// This file is part of the warp library.
// 
// The warp library is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by the
// Free Software Foundation; either version 2 of the License, or any later
// version.
// 
// The warp library is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
// Public License for more details.
// 
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
//----------------------------------------------------------------------------

#ifndef WARP_TMPFILE_H
#define WARP_TMPFILE_H

#include <warp/file.h>
#include <warp/generator.h>
#include <string>

namespace warp
{
    //------------------------------------------------------------------------
    // TmpFileGen
    //------------------------------------------------------------------------
    class TmpFileGen : public Generator<FilePtr>
    {
        std::string rootDir;
        bool madeDir;

    public:
        TmpFileGen(std::string const & rootDir);
        FilePtr next();
    };

    /// Open a unique temporary file in the given directory.  The
    /// filename will be determined automatically, but the prefix can
    /// be specified.  The directory must reside on a local
    /// filesystem.
    FilePtr openTmpFile(std::string const & dir, std::string const & prefix);
}

#endif // WARP_TMPFILE_H
