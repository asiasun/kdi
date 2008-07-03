//---------------------------------------------------------- -*- Mode: C++ -*-
// Copyright (C) 2006 Josh Taylor (Kosmix Corporation)
// Created 2006-08-18
//
// This file is part of KDI.
// 
// KDI is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// any later version.
// 
// KDI is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
// or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
// License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
// 02110-1301, USA.
//----------------------------------------------------------------------------

#include "init.h"
#include "warp/init.h"

void oort::initModule()
{
    static bool once = false;
    if(!once)
    {
        once = true;
        
        warp::initModule();

        WARP_CALL_INIT(oort_recordstream);
        WARP_CALL_INIT(oort_splitstream);
        WARP_CALL_INIT(oort_checkstream);
    }
}
