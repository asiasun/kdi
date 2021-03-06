//---------------------------------------------------------- -*- Mode: C++ -*-
// Copyright (C) 2008 Josh Taylor (Kosmix Corporation)
// Created 2008-03-17
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

#ifndef WARP_TUPLE_ENCODE_H
#define WARP_TUPLE_ENCODE_H

#include <warp/string_range.h>
#include <warp/zero_escape.h>
#include <warp/tuple.h>
#include <ex/exception.h>
#include <boost/range/iterator_range.hpp>
#include <algorithm>
#include <vector>
#include <string>
#include <sstream>

//----------------------------------------------------------------------------
// String sequence encoding
//----------------------------------------------------------------------------
namespace warp {

    /// An ostream adapter that will encode a sequence of strings as a
    /// single string, maintaining nested lexicographical order.
    template <class StringIt>
    class StringSequenceEncoder :
        public boost::iterator_range<StringIt>
    {
        typedef boost::iterator_range<StringIt> super;
    public:
        StringSequenceEncoder(StringIt begin, StringIt end) : super(begin,end) {}
    };

    template <class StringIt>
    std::ostream & operator<<(
        std::ostream & s, StringSequenceEncoder<StringIt> const & x)
    {
        for(StringIt i = x.begin(); i != x.end(); ++i)
            s << ZeroEscape(*i) << '\0' << '\0';
        return s;
    }

    /// Encode a sequence of strings into a single string in such a
    /// way that lexicographical ordering is preserved.
    template <class StringIt>
    std::string encodeStringSequence(StringIt begin, StringIt end)
    {
        std::ostringstream oss;
        oss << StringSequenceEncoder<StringIt>(begin, end);
        return oss.str();
    }

    /// Encode a sequence of strings into a single string in such a
    /// way that lexicographical ordering is preserved.
    template <class StringContainer>
    std::string encodeStringSequence(StringContainer const & c)
    {
        return encodeStringSequence(c.begin(), c.end());
    }

} // namespace warp


//----------------------------------------------------------------------------
// String sequence decoding
//----------------------------------------------------------------------------
namespace warp {
    
    /// Decode a string generated by encodeStringSequence into the
    /// original sequence of strings.
    template <class StringIt>
    StringIt decodeStringSequence(strref_t encoded, StringIt outIt)
    {
        char const * p = encoded.begin();
        char const * end = encoded.end();
        while(p != end)
        {
            char const * pp = std::search_n(p, end, 2, '\0');
            if(pp == end)
            {
                using namespace ex;
                raise<ValueError>("invalid tuple-encoded string");
            }

            *outIt = zeroUnescape(StringRange(p, pp));
            ++outIt;
            p = pp + 2;
        }
        return outIt;
    }

    /// Decode a string generated by encodeStringSequence into the
    /// original sequence of strings.
    inline
    std::vector<std::string> decodeStringSequence(strref_t encoded)
    {
        std::vector<std::string> items;
        decodeStringSequence(encoded, std::back_inserter(items));
        return items;
    }

} // namespace warp


//----------------------------------------------------------------------------
// Tuple encoding
//----------------------------------------------------------------------------
namespace warp {

    /// An ostream adapter that will encode a tuple of strings as a
    /// single string, maintaining nested lexicographical order.
    template <class T>
    class TupleEncoder
    {
    public:
        explicit TupleEncoder(T const & tuple) : tuple(tuple) {}
        T const & tuple;
    };
    
    template <class T>
    std::ostream & operator<<(std::ostream & o, TupleEncoder<T> const & x)
    {
        return o << ZeroEscape(x.tuple.get_head()) << '\0' << '\0'
                 << TupleEncoder<typename T::tail_type>(x.tuple.get_tail());
    }

    template <> inline
    std::ostream & operator<<(std::ostream & o, TupleEncoder< tuple<> > const &)
    {
        return o;
    }

    template <> inline
    std::ostream & operator<<(std::ostream & o, TupleEncoder< null_type > const &)
    {
        return o;
    }

    /// Encode a tuple of strings as a single string.  The encoded
    /// string maintains the lexicographical ordering of the original
    /// tuple.
    template <class T>
    std::string encodeTuple(T const & tuple)
    {
        std::ostringstream oss;
        oss << TupleEncoder<T>(tuple);
        return oss.str();
    }

} // namespace warp


//----------------------------------------------------------------------------
// Tuple decoding
//----------------------------------------------------------------------------
namespace warp {

    namespace details {

        template <class T>
        void decodeTuple(strref_t encoded, T const & tuple,
                         std::ostringstream & oss)
        {
            char const * p = std::search_n(
                encoded.begin(), encoded.end(), 2, '\0');
            if(p == encoded.end())
            {
                using namespace ex;
                raise<ValueError>("decode tuple of wrong size");
            }

            oss.str(std::string());
            oss << ZeroUnescape(StringRange(encoded.begin(), p));
            tuple.get_head() = oss.str();

            decodeTuple(StringRange(p+2, encoded.end()),
                        tuple.get_tail(), oss);
        }

        template <> inline
        void decodeTuple(strref_t encoded, tuple<> const & tuple,
                         std::ostringstream & oss)
        {
            if(encoded)
            {
                using namespace ex;
                raise<ValueError>("decode tuple of wrong size");
            }
        }

        template <> inline
        void decodeTuple(strref_t encoded, null_type const & tuple,
                         std::ostringstream & oss)
        {
            if(encoded)
            {
                using namespace ex;
                raise<ValueError>("decode tuple of wrong size");
            }
        }

    }

    /// Decode a tuple previously encoded with encodeTuple or
    /// encodeStringSequence.  The tuple size must match the encoded
    /// sequence.  If the size is uncertain, use decodeStringSequence.
    template <class T>
    void decodeTuple(strref_t encoded, T const & tuple)
    {
        std::ostringstream oss;
        details::decodeTuple(encoded, tuple, oss);
    }

} // namespace warp

#endif // WARP_TUPLE_ENCODE_H
