//---------------------------------------------------------- -*- Mode: C++ -*-
// Copyright (C) 2009 Josh Taylor (Kosmix Corporation)
// Created 2009-04-07
//
// This file is part of KDI.
//
// KDI is free software; you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation; either version 2 of the License, or any later version.
//
// KDI is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
// details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
//----------------------------------------------------------------------------

#include <kdi/server/ScannerI.h>
#include <kdi/server/ScannerLocator.h>
#include <warp/log.h>

using namespace kdi;
using namespace kdi::server;
using warp::log;

//----------------------------------------------------------------------------
// ScannerI::ScanCb
//----------------------------------------------------------------------------
class ScannerI::ScanCb
    : public ::kdi::server::Scanner::ScanCb
{
public:
    ScanCb(ScannerI * scannerI, RpcScanMoreCbPtr const & cb,
           bool forceClose) :
        scannerI(scannerI),
        cb(cb),
        forceClose(forceClose)
    {
    }
    
    void done()
    {
        ::kdi::server::ScannerPtr const & scanner = scannerI->scanner;

        strref_t cells = scanner->getPackedCells();

        ::kdi::rpc::ScanResult result;
        result.scanTxn = scanner->getScanTransaction();
        result.scanComplete = !scanner->scanContinues();
        result.scanClosed = (result.scanComplete || forceClose);

        if(result.scanClosed)
            scannerI->doClose();

        cb->ice_response(
            RpcPackedCells(
                reinterpret_cast<Ice::Byte const *>(cells.begin()),
                reinterpret_cast<Ice::Byte const *>(cells.end())),
            result);

        resetUseFlag();
        delete this;
    }

    void error(std::exception const & err)
    {
        cb->ice_exception(err);
        resetUseFlag();
        delete this;
    }

private:
    void resetUseFlag()
    {
        boost::mutex::scoped_lock lock(scannerI->mutex);
        scannerI->inUse = false;
    }

private:
    ScannerI * scannerI;
    RpcScanMoreCbPtr cb;
    bool forceClose;
};


//----------------------------------------------------------------------------
// ScannerI
//----------------------------------------------------------------------------
ScannerI::ScannerI(::kdi::server::ScannerPtr const & scanner,
                   ::kdi::server::ScannerLocator * locator,
                   size_t scanId) :
    scanner(scanner),
    locator(locator),
    scanId(scanId),
    inUse(false)
{
}

void ScannerI::scanMore_async(RpcScanMoreCbPtr const & cb,
                              RpcScanParams const & params,
                              Ice::Current const & cur)
{
    log("ScannerI %d: scanMore", scanId);

    boost::mutex::scoped_lock lock(mutex);
    if(inUse)
    {
        cb->ice_exception(::kdi::rpc::ScannerBusyError());
        return;
    }
    inUse = true;
    lock.unlock();

    scanner->scan_async(
        new ScanCb(this, cb, params.close),
        params.maxCells,
        params.maxSize);
}

void ScannerI::close_async(RpcCloseCbPtr const & cb,
                           Ice::Current const & cur)
{
    log("ScannerI %d: close", scanId);

    boost::mutex::scoped_lock lock(mutex);
    if(inUse)
    {
        cb->ice_exception(::kdi::rpc::ScannerBusyError());
        return;
    }

    doClose();

    cb->ice_response();
}

void ScannerI::doClose()
{
    locator->remove(scanId);
}
