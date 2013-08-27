#include <boost/python.hpp>

#include "HOOMDMath.h"
#include "Index1D.h"
#include "num_util.h"

#ifndef _SPLIT_H__
#define _SPLIT_H__

namespace freud { namespace viz {

/*! \file split.cc
    \brief Helper routines for splitting particles
*/

void split(float3 *split_array,
              float *sangle_array,
              const float3 *position_array,
              const float *angle_array,
              const float2 *centers_array,
              unsigned int N,
              unsigned int NS);

float2 rotate(float2 point, float angle);

void splitPy(boost::python::numeric::array split_array,
                boost::python::numeric::array sangle_array,
                boost::python::numeric::array position_array,
                boost::python::numeric::array angle_array,
                boost::python::numeric::array centers_array
                );

/*! \internal
    \brief Exports all classes in this file to python
*/
void export_split();

} } // end namespace freud::viz

#endif