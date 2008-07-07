//---------------------------------------------------------- -*- Mode: C++ -*-
// Copyright (C) 2006 Josh Taylor (Kosmix Corporation)
// Created 2006-02-17
// 
// This file is part of the oort library.
// 
// The oort library is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by the
// Free Software Foundation; either version 2 of the License, or any later
// version.
// 
// The oort library is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
// Public License for more details.
// 
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
//----------------------------------------------------------------------------

#include <oort/headers.h>

namespace
{
    oort::vega::RecordHeader::Spec const vegaSpec;
    oort::sirius::RecordHeader::Spec const siriusSpec;
}

oort::vega::RecordHeader::Spec const * const oort::VEGA_SPEC = &vegaSpec;
oort::sirius::RecordHeader::Spec const * const oort::SIRIUS_SPEC = &siriusSpec;
