//=========================================================================
//  XYARRAY.H - part of
//                  OMNeT++/OMNEST
//           Discrete System Simulation in C++
//
//=========================================================================

/*--------------------------------------------------------------*
  Copyright (C) 1992-2017 Andras Varga
  Copyright (C) 2006-2017 OpenSim Ltd.

  This file is distributed WITHOUT ANY WARRANTY. See the file
  `license' for details on this and other legal matters.
*--------------------------------------------------------------*/

#ifndef __OMNETPP_SCAVE_XYARRAY_H
#define __OMNETPP_SCAVE_XYARRAY_H

#include "scavedefs.h"

namespace omnetpp {
namespace scave {

/**
 * ArrayBuilderNode returns the results in this class.
 */
class SCAVE_API XYArray
{
    public:
        std::vector<double> xs;
        std::vector<double> ys;
        std::vector<BigDecimal> xps;
        std::vector<eventnumber_t> ens;
    public:
        XYArray(std::vector<double> &&xs, std::vector<double> &&ys, std::vector<BigDecimal> &&xps = std::vector<BigDecimal>(), std::vector<eventnumber_t> &&ens = std::vector<eventnumber_t>());

        XYArray() {}
        XYArray(const XYArray&) = delete;
        XYArray(XYArray&&) = default;

        bool hasPreciseX() const  {return !xps.empty();}
        bool hasEventNumbers() const  {return !ens.empty();}
        size_t length() const  {return xs.size();}

        double getX(size_t i) const  {return xs.at(i);}
        double getY(size_t i) const  {return ys.at(i);}
        const BigDecimal& getPreciseX(size_t i) const {return xps.at(i); }
        eventnumber_t getEventNumber(size_t i) const {return ens.at(i); }
};

}  // namespace scave
}  // namespace omnetpp


#endif
