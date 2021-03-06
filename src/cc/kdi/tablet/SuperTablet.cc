//---------------------------------------------------------- -*- Mode: C++ -*-
// Copyright (C) 2008 Josh Taylor (Kosmix Corporation)
// Created 2008-07-22
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

#include <kdi/tablet/SuperTablet.h>
#include <kdi/tablet/Tablet.h>
#include <kdi/tablet/TabletName.h>
#include <kdi/tablet/TabletConfig.h>
#include <kdi/tablet/WorkQueue.h>
#include <kdi/tablet/SuperScanner.h>
#include <kdi/tablet/SharedCompactor.h>
#include <kdi/cell_filter.h>
#include <kdi/scan_predicate.h>
#include <warp/interval.h>
#include <warp/log.h>
#include <boost/format.hpp>
#include <boost/bind.hpp>

using namespace kdi;
using namespace kdi::tablet;
using namespace warp;
using namespace ex;
using namespace std;
using boost::format;

namespace {

    struct TabletLt
    {
        warp::IntervalPointOrder<warp::less> lt;

        bool operator()(TabletPtr const & a, TabletPtr const & b)
        {
            return lt(a->getLastRow(),
                      b->getLastRow());
        }

        bool operator()(TabletPtr const & a, IntervalPoint<string> const & b)
        {
            return lt(a->getLastRow(), b);
        }

        bool operator()(IntervalPoint<string> const & a, TabletPtr const & b)
        {
            return lt(a, b->getLastRow());
        }

        bool operator()(TabletPtr const & a, strref_t b)
        {
            return lt(a->getLastRow(), b);
        }

        bool operator()(strref_t a, TabletPtr const & b)
        {
            return lt(a, b->getLastRow());
        }
    };

    template <class It, class T>
    TabletPtr const & getTabletInternal(It begin, It end, T const & row)
    {
        // Find tablet such that its upper bound contains the row
        TabletLt tlt;
        It i = std::lower_bound(begin, end, row, tlt);

        // If the tablet lower bound also contains the row, we have a
        // match
        if(i != end && !tlt.lt(row, (*i)->getRows().getLowerBound()))
            return *i;
        else
            raise<ValueError>("row not on this server: %s", row);
    }

    struct ReopenSuperScannerOrRemove
    {
        /// Reopen the scanner if the handle is still valid.  If not,
        /// return true, as it should be removed from the list.
        bool operator()(SuperScannerWeakPtr const & weakScanner) const
        {
            SuperScannerPtr scanner = weakScanner.lock();
            if(scanner)
            {
                scanner->reopen();
                return false;   // don't remove scanner
            }
            else
                return true;    // remove expired scanner
        }
    };

}

//----------------------------------------------------------------------------
// SuperTablet::MutationInterlock
//----------------------------------------------------------------------------
class SuperTablet::MutationInterlock
{
    SuperTablet & x;
    
public:
    explicit MutationInterlock(SuperTablet & x) : x(x)
    {
        lock_t lock(x.mutex);
        while(x.mutationsBlocked)
            x.allowMutations.wait(lock);
        ++x.mutationsPending;
    }

    ~MutationInterlock()
    {
        lock_t lock(x.mutex);
        if(--x.mutationsPending == 0)
            x.allQuiet.notify_all();
    }
};


//----------------------------------------------------------------------------
// SuperTablet::MutationLull
//----------------------------------------------------------------------------
class SuperTablet::MutationLull
{
    SuperTablet & x;
    
public:
    explicit MutationLull(SuperTablet & x) : x(x)
    {
        lock_t lock(x.mutex);
        x.mutationsBlocked = true;
        while(x.mutationsPending)
            x.allQuiet.wait(lock);
    }

    ~MutationLull()
    {
        lock_t lock(x.mutex);
        x.mutationsBlocked = false;
        x.allowMutations.notify_all();
    }
};


//----------------------------------------------------------------------------
// SuperTablet
//----------------------------------------------------------------------------
SuperTablet::SuperTablet(std::string const & name,
                         MetaConfigManagerPtr const & configMgr,
                         FragmentLoader * loader,
                         SharedLoggerPtr const & logger,
                         SharedCompactorPtr const & compactor,
                         FileTrackerPtr const & tracker,
                         WorkQueuePtr const & workQueue) :
    workQueue(workQueue),
    mutationsBlocked(false),
    mutationsPending(0)
{
    std::list<TabletConfig> cfgs = configMgr->loadTabletConfigs(name);
    
    if(cfgs.empty())
    {
        raise<TableDoesNotExistError>(
            "Table has no tablets on this server: %s",
            name);
    }

    SharedCompactor::Pause pause(*compactor);

    for(std::list<TabletConfig>::const_iterator i = cfgs.begin();
        i != cfgs.end(); ++i)
    {
        tablets.push_back(
            Tablet::make(
                name,
                configMgr,
                loader,
                logger,
                compactor,
                tracker,
                workQueue,
                *i,
                this));
    }
}

SuperTablet::~SuperTablet()
{
}

void SuperTablet::set(strref_t row, strref_t column, int64_t timestamp,
                      strref_t value)
{
    MutationInterlock interlock(*this);
    getTablet(row)->set(row,column,timestamp,value);
}

void SuperTablet::erase(strref_t row, strref_t column, int64_t timestamp)
{
    MutationInterlock interlock(*this);
    getTablet(row)->erase(row,column,timestamp);
}

void SuperTablet::insert(Cell const & x)
{
    MutationInterlock interlock(*this);
    getTablet(x.getRow())->insert(x);
}

CellStreamPtr SuperTablet::scan(ScanPredicate const & pred) const
{
    SuperScannerPtr scanner;
    CellStreamPtr output;

    // Put history filter on outside
    if(pred.getMaxHistory())
    {
        ScanPredicate p(pred);
        p.setMaxHistory(0);
        scanner.reset(new SuperScanner(shared_from_this(), p));
        output = makeHistoryFilter(pred.getMaxHistory());
        output->pipeFrom(scanner);
    }
    else
    {
        scanner.reset(new SuperScanner(shared_from_this(), pred));
        output = scanner;
    }

    {
        lock_t lock(scannerMutex);
        scanners.push_back(scanner);
    }

    return output;
}

void SuperTablet::sync()
{
    MutationInterlock interlock(*this);
    for(vector<TabletPtr>::const_iterator i = tablets.begin();
        i != tablets.end(); ++i)
    {
        //log("SuperTablet::sync(): %s", (*i)->getPrettyName());
        (*i)->sync();
    }
}

void SuperTablet::requestSplit(Tablet * tablet)
{
    workQueue->post(
        boost::bind(
            &SuperTablet::performSplit,
            shared_from_this(),
            tablet
            )
        );
}

void SuperTablet::performSplit(Tablet * tablet)
{
    // Stop mutations for this duration of this call
    MutationLull lull(*this);

    // We want to make sure access to tablet vector is safe.  There
    // could be scanners trying to open through scanFirstTablet(), so
    // we need to lock.  The call to splitTablet() will shrink the
    // tablet's row range.  This will leave a gap until the new tablet
    // is returned and put into the vector.  It's not safe to allow
    // access in that time.
    lock_t lock(mutex);

    // Split the tablet at the given row
    TabletPtr lowTablet = tablet->splitTablet();
    
    // We may not get a new tablet if there was no place to split
    if(!lowTablet)
        return;

    // Insert the tablet into our map
    vector<TabletPtr>::iterator it = std::lower_bound(
        tablets.begin(), tablets.end(), lowTablet, TabletLt());
    tablets.insert(it, lowTablet);

    // Done with tablets
    lock.unlock();

    // Reopen scanners
    updateScanners();
}

CellStreamPtr SuperTablet::scanFirstTablet(
    ScanPredicate const & pred,
    warp::Interval<std::string> * tabletRows) const
{
    assert(pred.getRowPredicate() && !pred.getRowPredicate()->isEmpty());

    lock_t lock(mutex);

    TabletPtr tablet = getTabletInternal(tablets.begin(), tablets.end(),
                                         *pred.getRowPredicate()->begin());

    Interval<string> rows = tablet->getRows();
    if(tabletRows)
        *tabletRows = rows;

    return tablet->scan(pred.clipRows(rows));
}

TabletPtr const & SuperTablet::getTablet(strref_t row) const
{
    lock_t lock(mutex);
    return getTabletInternal(tablets.begin(), tablets.end(), row);
}

void SuperTablet::updateScanners()
{
    lock_t lock(scannerMutex);

    // Reopen all valid scanners, remove all null scanners.
    scanners.erase(
        std::remove_if(
            scanners.begin(),
            scanners.end(),
            ReopenSuperScannerOrRemove()),
        scanners.end());
}
