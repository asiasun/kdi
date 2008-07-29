//---------------------------------------------------------- -*- Mode: C++ -*-
// Copyright (C) 2008 Josh Taylor (Kosmix Corporation)
// Created 2008-05-14
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

#include <kdi/tablet/Tablet.h>
#include <kdi/tablet/TabletConfig.h>
#include <kdi/tablet/Scanner.h>
#include <kdi/tablet/ConfigManager.h>
#include <kdi/tablet/SharedCompactor.h>
#include <kdi/tablet/SharedLogger.h>
#include <kdi/scan_predicate.h>
#include <kdi/cell_filter.h>
#include <kdi/cell_merge.h>
#include <warp/config.h>
#include <warp/functional.h>
#include <warp/algorithm.h>
#include <warp/fs.h>
#include <warp/uri.h>
#include <warp/log.h>
#include <ex/exception.h>
#include <boost/format.hpp>

#include <kdi/local/disk_table_writer.h>
using kdi::local::DiskTableWriter;

using namespace kdi;
using namespace kdi::tablet;
using namespace warp;
using namespace ex;
using namespace std;
using boost::format;

namespace {

    /// Make sure row is in tablet range or raise a RowNotInTabletError
    inline void validateRow(strref_t row, Interval<string> const & rows)
    {
        if(!rows.contains(row, warp::less()))
            raise<RowNotInTabletError>("%s", row);
    }

    /// Make sure row set is in tablet range or raise a RowNotInTabletError
    inline void validateRows(ScanPredicate::StringSetCPtr const & rowSet,
                             Interval<string> const & rows)
    {
        if(rowSet)
        {
            if(!rows.contains(rowSet->getHull()))
                raise<RowNotInTabletError>("%s", *rowSet);
        }
        else if(!rows.isInfinite())
            raise<RowNotInTabletError>("<< >>");
    }
}

//----------------------------------------------------------------------------
// Tablet
//----------------------------------------------------------------------------
Tablet::Tablet(std::string const & tableName,
               ConfigManagerPtr const & configMgr,
               SharedLoggerPtr const & logger,
               SharedCompactorPtr const & compactor,
               TabletConfig const & cfg) :
    tableName(tableName),
    configMgr(configMgr),
    logger(logger),
    compactor(compactor),
    mutationsPending(false),
    configChanged(false)
{
    log("Tablet %p %s: created", this, getTableName());

    EX_CHECK_NULL(configMgr);
    EX_CHECK_NULL(logger);
    EX_CHECK_NULL(compactor);

    loadConfig(cfg);
}

Tablet::~Tablet()
{
    log("Tablet %p %s: destroyed", this, getTableName());
}

void Tablet::set(strref_t row, strref_t column, int64_t timestamp, strref_t value)
{
    validateRow(row, rows);
    logger->set(shared_from_this(), row, column, timestamp, value);
    mutationsPending = true;
}

void Tablet::erase(strref_t row, strref_t column, int64_t timestamp)
{
    validateRow(row, rows);
    logger->erase(shared_from_this(), row, column, timestamp);
    mutationsPending = true;
}
    
void Tablet::sync()
{
    if(mutationsPending)
    {
        logger->sync();
        mutationsPending = false;
    }

    lock_t lock(statusMutex);
    if(configChanged)
    {
        saveConfig();
        configChanged = false;
    }
}

CellStreamPtr Tablet::scan() const
{
    return scan(ScanPredicate());
}

CellStreamPtr Tablet::scan(ScanPredicate const & pred) const
{
    // Make sure this is a valid scan for this tablet
    validateRows(pred.getRowPredicate(), rows);

    // Strip the history predicate -- we'll implement it as a
    // filter after everything else
    ScanPredicate p(pred);
    int history = p.getMaxHistory();
    p.setMaxHistory(0);

    // Create a new scanner and keep track of it
    ScannerPtr scanner(new Scanner(shared_from_this(), p));
    LockedPtr<scanner_vec_t>(syncScanners)->push_back(scanner);

    // Add the history filter if we need it and return
    if(history)
    {
        CellStreamPtr filter = makeHistoryFilter(history);
        filter->pipeFrom(scanner);
        return filter;
    }
    else
        return scanner;
}

std::string Tablet::getPrettyName() const
{
    ostringstream oss;
    oss << "table=" << reprString(tableName)
        << " last=";
    if(rows.getUpperBound().isFinite())
        oss << reprString(rows.getUpperBound().getValue());
    else
        oss << "END";
    return oss.str();
}

CellStreamPtr Tablet::getMergedScan(ScanPredicate const & pred) const
{
    // We don't support history predicates
    if(pred.getMaxHistory())
        raise<ValueError>("unsupported history predicate: %s", pred);

    // Get exclusive access to table list
    LockedPtr<tables_t const> tables(syncTables);

    // If there is only one table, no merge is necessary but we still
    // have to filter erasures.  The one exception to this is if the
    // single table is fully-compacted disk table.
    if(tables->size() == 1)
    {
        if(tables->front().isStatic)
        {
            // This is a compacted disk table.  It should contain no
            // erasures.
            return tables->front().table->scan(pred);
        }
        else
        {
            // The table may contain erasures.
            CellStreamPtr filter = makeErasureFilter();
            filter->pipeFrom(tables->front().table->scan(pred));
            return filter;
        }
    }

    // XXX may need to block if the merge width is too great

    // Need to build a merge stream (note that a 0-way merge is
    // possible and well-formed).  Tables added to the merge in
    // reverse order so later streams override earlier streams.
    CellStreamPtr merge = CellMerge::make(true);
    for(tables_t::const_reverse_iterator i = tables->rbegin();
        i != tables->rend(); ++i)
    {
        merge->pipeFrom(i->table->scan(pred));
    }
    return merge;
}

void Tablet::loadConfig(TabletConfig const & cfg)
{
    // Grab the server name and row interval
    server = cfg.getServer();
    rows = cfg.getTabletRows();

    // Load our tables
    bool uriChanged = false;
    {
        LockedPtr<tables_t> tables(syncTables);
        tables->clear();
        vector<string> const & uris = cfg.getTableUris();
        for(vector<string>::const_iterator i = uris.begin();
            i != uris.end(); ++i)
        {
            std::pair<TablePtr, string> p = configMgr->openTable(*i);
            tables->push_back(TableInfo(p.first, p.second, true));
        
            if(p.second != *i)
                uriChanged = true;
        }
    }

    // If a table URI changed during the load, resave our config
    if(uriChanged)
    {
        log("Tablet config changed on load, saving: %s", getPrettyName());
        saveConfig();
    }
}

void Tablet::saveConfig() const
{
    // Build list of tables
    vector<string> uris;
    {
        LockedPtr<tables_t const> tables(syncTables);
        uris.reserve(tables->size());
        for(tables_t::const_iterator i = tables->begin();
            i != tables->end(); ++i)
        {
            uris.push_back(i->uri);
        }
    }

    // Save our config
    TabletConfig cfg(rows, uris, server);
    configMgr->setTabletConfig(tableName, cfg);
}

void Tablet::addTable(TablePtr const & table, std::string const & tableUri)
{
    EX_CHECK_NULL(table);

    log("Tablet %s: add table (%p) %s", getTableName(), table.get(), tableUri);

    bool wantCompaction = false;
    {
        // Lock tables
        LockedPtr<tables_t> tables(syncTables);

        // Add to table list
        tables->push_back(TableInfo(table, tableUri, false));

        // Let's just say we want a compaction if we have more than 5
        // tables (or more than 4 static tables)
        wantCompaction = (tables->size() > 5);
    }

    // Note that config has changed
    {
        lock_t lock(statusMutex);
        configChanged = true;
    }

    // Request a compaction if we need one
    if(wantCompaction)
        compactor->requestCompaction(shared_from_this());
}

void Tablet::replaceTables(std::vector<TablePtr> const & oldTables,
                           TablePtr const & newTable,
                           std::string const & newTableUri)
{
    // Check args
    if(oldTables.empty())
        raise<ValueError>("replaceTables with empty table sequence");
    EX_CHECK_NULL(newTable);

    log("Tablet %s: replace %d tables with (%p) %s", getTableName(),
        oldTables.size(), newTable.get(), newTableUri);

    vector<string> deadFiles;
    {
        // Lock tableSet
        LockedPtr<tables_t> tables(syncTables);

        // Find old sequence in table vector
        tables_t::iterator i = std::search(
            tables->begin(), tables->end(), oldTables.begin(), oldTables.end());
        if(i != tables->end())
        {
            // Mark first table for deletion
            if(uriTopScheme(i->uri) == "disk")
                deadFiles.push_back(uriPopScheme(i->uri));

            // Replace first item in the sequence
            *i = TableInfo(newTable, newTableUri, true);

            // Mark rest of the old tables for deletion
            tables_t::iterator beginErase = ++i;
            for(size_t n = 1; n < oldTables.size(); ++n, ++i)
            {
                if(uriTopScheme(i->uri) == "disk")
                    deadFiles.push_back(uriPopScheme(i->uri));
            }
            
            // Remove the rest of the tables from the set
            tables->erase(beginErase, i);
        }
        else
        {
            // We didn't know about that sequence
            raise<RuntimeError>("replaceTables with unknown table sequence");
        }
    }

    // Save new config
    saveConfig();

    // Update scanners
    updateScanners();

    // Remove old files
    std::for_each(deadFiles.begin(), deadFiles.end(), fs::remove);

    // XXX: Check to see if we should split
    // if(getDiskSize() >= SPLIT_THRESHOLD)
    // {
    //     std::string splitRow = chooseSplitRow();
    //     configMgr->suggestSplit(tableName, splitRow);
    // }
}

namespace
{
    struct ReopenScannerOrRemove
    {
        /// Reopen the scanner if the handle is still valid.  If not,
        /// return true, as it should be removed from the list.
        bool operator()(ScannerWeakPtr const & weakScanner) const
        {
            ScannerPtr scanner = weakScanner.lock();
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

size_t Tablet::getCompactionPriority() const
{
    LockedPtr<tables_t const> tables(syncTables);

    // Get the number of static tables in the set
    size_t n = tables->size();
    if(n && !tables->back().isStatic)
        --n;
    
    // Return the number of static tables we have (don't bother if
    // there's only one)
    return (n > 1 ? n : 0);
}

void Tablet::doCompaction()
{
    vector<TablePtr> compactionTables;
    bool filterErasures = false;

    log("Tablet %s: start compaction", getTableName());
    
    // Choose the sequence of tables to compact
    {
        // Just compact the last K static tables -- maybe do something
        // fancy later

        int const K = 8;

        LockedPtr<tables_t const> tables(syncTables);
       
        // Find the last static table
        tables_t::const_iterator last = tables->begin();
        for(tables_t::const_reverse_iterator i = tables->rbegin(); i != tables->rend(); ++i)
        {
            if(i->isStatic)
            {
                last = i.base();
                break;
            }
        }
        // Get the beginning of the trailing range.  We want to filter
        // erasures if our compaction includes the first table.
        tables_t::const_iterator first = tables->begin();
        if(distance(first, last) > K)
            advance(first, distance(first, last) - K);
        else
            filterErasures = true;

        // Make a copy of the table pointers to be compacted
        compactionTables.reserve(K);
        for(; first != last; ++first)
            compactionTables.push_back(first->table);
    }

    // Get an output file
    std::string fn = configMgr->getDataFile(tableName);
            
    // Open a DiskTable for writing
    DiskTableWriter writer(fn, 64<<10);

    // Build compaction merge stream
    CellStreamPtr merge = CellMerge::make(filterErasures);
    for(vector<TablePtr>::const_reverse_iterator i = compactionTables.rbegin();
        i != compactionTables.rend(); ++i)
    {
        merge->pipeFrom((*i)->scan());
    }

    // Compact the tables
    Cell x;
    while(merge->get(x))
        writer.put(x);
    writer.close();     // XXX should sync file

    log("Tablet %s: end compaction", getTableName());

    // Reopen the table for reading
    std::string diskUri = uriPushScheme(fn, "disk");
    std::pair<TablePtr,string> p = configMgr->openTable(diskUri);


    // Update table set
    replaceTables(compactionTables, p.first, p.second);
}

void Tablet::updateScanners() const
{
    LockedPtr<scanner_vec_t> scanners(syncScanners);

    // Reopen all valid scanners, remove all null scanners.
    scanners->erase(
        std::remove_if(
            scanners->begin(),
            scanners->end(),
            ReopenScannerOrRemove()),
        scanners->end());
}
