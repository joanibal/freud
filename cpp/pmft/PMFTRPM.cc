#include "PMFTRPM.h"
#include "ScopedGILRelease.h"

#include <stdexcept>
#ifdef __SSE2__
#include <emmintrin.h>
#endif

#ifdef ENABLE_OPENMP
#include <omp.h>
#endif

#include <tbb/tbb.h>

#include "VectorMath.h"

using namespace std;
using namespace boost::python;

using namespace tbb;

/*! \file PMFTRPM.cc
    \brief Routines for computing radial density functions
*/

namespace freud { namespace pmft {

PMFTRPM::PMFTRPM(const trajectory::Box& box, float max_r, float max_TP, float max_TM, float dr, float dTP, float dTM)
    : m_box(box), m_max_r(max_r), m_max_TP(max_TP), m_max_TM(max_TM), m_dr(dr), m_dTP(dTP), m_dTM(dTM)
    {
    if (dr < 0.0f)
        throw invalid_argument("dr must be positive");
    if (dTP < 0.0f)
        throw invalid_argument("dTP must be positive");
    if (dTM < 0.0f)
        throw invalid_argument("dTM must be positive");
    if (max_r < 0.0f)
        throw invalid_argument("max_r must be positive");
    if (max_TP < 0.0f)
        throw invalid_argument("max_TP must be positive");
    if (max_TM < 0.0f)
        throw invalid_argument("max_TM must be positive");
    if (dr > max_r)
        throw invalid_argument("max_r must be greater than dr");
    if (dTP > max_TP)
        throw invalid_argument("max_TP must be greater than dTP");
    if (dTM > max_TM)
        throw invalid_argument("max_TM must be greater than dTM");
    if (max_r > box.getLx()/2 || max_r > box.getLy()/2)
        throw invalid_argument("max_r, max_r must be smaller than half the smallest box size");
    if (!box.is2D())
        throw invalid_argument("box must be 2D");

    m_nbins_r = int(2 * floorf(m_max_r / m_dr));
    assert(m_nbins_r > 0);
    m_nbins_TP = int(2 * floorf(m_max_TP / m_dTP));
    assert(m_nbins_TP > 0);
    m_nbins_TM = int(2 * floorf(m_max_TM / m_dTM));
    assert(m_nbins_TM > 0);

    // precompute the bin center positions for r, TP, TM
    m_r_array = boost::shared_array<float>(new float[m_nbins_r]);
    for (unsigned int i = 0; i < m_nbins_r; i++)
        {
        float r = float(i) * m_dr;
        float nextr = float(i+1) * m_dr;
        m_r_array[i] = 2.0f / 3.0f * (nextr*nextr*nextr - r*r*r) / (nextr*nextr - r*r);
        }

    m_TP_array = boost::shared_array<float>(new float[m_nbins_TP]);
    for (unsigned int i = 0; i < m_nbins_TP; i++)
        {
        float TP = float(i) * m_dTP;
        float nextTP = float(i+1) * m_dTP;
        m_TP_array[i] = -m_max_TP + ((TP + nextTP) / 2.0);
        }

    m_TM_array = boost::shared_array<float>(new float[m_nbins_TM]);
    for (unsigned int i = 0; i < m_nbins_TM; i++)
        {
        float TM = float(i) * m_dTM;
        float nextTM = float(i+1) * m_dTM;
        m_TM_array[i] = -m_max_TM + ((TM + nextTM) / 2.0);
        }

    if (useCells())
        {
        m_lc = new locality::LinkCell(box, max_r);
        }
    }

PMFTRPM::~PMFTRPM()
    {
    if(useCells())
    delete m_lc;
    }

class ComputePMFTRPMWithoutCellList
    {
    private:
        atomic<unsigned int> *m_pcf_array;
        unsigned int m_nbins_r;
        unsigned int m_nbins_TP;
        unsigned int m_nbins_TM;
        const trajectory::Box m_box;
        const float m_max_r;
        const float m_max_TP;
        const float m_max_TM;
        const float m_dr;
        const float m_dTP;
        const float m_dTM;
        const float3 *m_ref_points;
        float *m_ref_orientations;
        const unsigned int m_Nref;
        const float3 *m_points;
        float *m_orientations;
        const unsigned int m_Np;
    public:
        ComputePMFTRPMWithoutCellList(atomic<unsigned int> *pcf_array,
                                   unsigned int nbins_r,
                                   unsigned int nbins_TP,
                                   unsigned int nbins_TM,
                                   const trajectory::Box &box,
                                   const float max_r,
                                   const float max_TP,
                                   const float max_TM,
                                   const float dr,
                                   const float dTP,
                                   const float dTM,
                                   const float3 *ref_points,
                                   float *ref_orientations,
                                   unsigned int Nref,
                                   const float3 *points,
                                   float *orientations,
                                   unsigned int Np)
            : m_pcf_array(pcf_array), m_nbins_r(nbins_r), m_nbins_TP(nbins_TP), m_nbins_TM(nbins_TM), m_box(box),
              m_max_r(max_r), m_max_TP(max_TP), m_max_TM(max_TM), m_dr(dr), m_dTP(dTP), m_dTM(dTM),
              m_ref_points(ref_points), m_ref_orientations(ref_orientations), m_Nref(Nref), m_points(points),
              m_orientations(orientations), m_Np(Np)
        {
        }
        void operator()( const blocked_range<size_t> &myR ) const
            {
            float dr_inv = 1.0f / m_dr;
            float maxrsq = m_max_r * m_max_r;
            float dTP_inv = 1.0f / m_dTP;
            float dTM_inv = 1.0f / m_dTM;

            // for each reference point
            for (size_t i = myR.begin(); i != myR.end(); i++)
                {
                float3 ref = m_ref_points[i];
                for (unsigned int j = 0; j < m_Np; j++)
                    {
                    float3 point = m_points[j];
                    float dx = float(point.x - ref.x);
                    float dy = float(point.y - ref.y);
                    float3 delta = m_box.wrap(make_float3(dx, dy, (float)0));

                    float rsq = delta.x*delta.x + delta.y*delta.y;
                    if (rsq < 1e-6)
                        {
                        continue;
                        }
                    if (rsq < maxrsq)
                        {
                        float r = sqrtf(rsq);
                        // calculate angles
                        float dTheta1 = atan2(delta.y, delta.x);
                        float dTheta2 = atan2(-delta.y, -delta.x);
                        float T1 = dTheta1 - m_ref_orientations[i];
                        float T2 = dTheta2 - m_orientations[j];
                        float TP = T1 + T2 + m_max_TP;
                        float TM = T1 - T2 + m_max_TM;

                        // bin that point
                        float binr = r * dr_inv;
                        float binTP = floorf(TP * dTP_inv);
                        float binTM = floorf(TM * dTM_inv);
                        // fast float to int conversion with truncation
                        #ifdef __SSE2__
                        unsigned int ibinr = _mm_cvtt_ss2si(_mm_load_ss(&binr));
                        unsigned int ibinTP = _mm_cvtt_ss2si(_mm_load_ss(&binTP));
                        unsigned int ibinTM = _mm_cvtt_ss2si(_mm_load_ss(&binTM));
                        #else
                        unsigned int ibinr = (unsigned int)(binr);
                        unsigned int ibinTP = (unsigned int)(binTP);
                        unsigned int ibinTM = (unsigned int)(binTM);
                        #endif

                        if ((ibinr < m_nbins_r) && (ibinTP < m_nbins_TP) && (ibinTM < m_nbins_TM))
                            {
                            m_pcf_array[ibinr*m_nbins_TP*m_nbins_TM + ibinTP*m_nbins_TM + ibinTM]++;
                            }
                        }
                    }
                } // done looping over reference points
            }
    };

class ComputePMFTRPMWithCellList
    {
    private:
        atomic<unsigned int> *m_pcf_array;
        unsigned int m_nbins_r;
        unsigned int m_nbins_TP;
        unsigned int m_nbins_TM;
        const trajectory::Box m_box;
        const float m_max_r;
        const float m_max_TP;
        const float m_max_TM;
        const float m_dr;
        const float m_dTP;
        const float m_dTM;
        const locality::LinkCell *m_lc;
        float3 *m_ref_points;
        float *m_ref_orientations;
        const unsigned int m_Nref;
        float3 *m_points;
        float *m_orientations;
        const unsigned int m_Np;
    public:
        ComputePMFTRPMWithCellList(atomic<unsigned int> *pcf_array,
                                unsigned int nbins_r,
                                unsigned int nbins_TP,
                                unsigned int nbins_TM,
                                const trajectory::Box &box,
                                const float max_r,
                                const float max_TP,
                                const float max_TM,
                                const float dr,
                                const float dTP,
                                const float dTM,
                                const locality::LinkCell *lc,
                                float3 *ref_points,
                                float *ref_orientations,
                                unsigned int Nref,
                                float3 *points,
                                float *orientations,
                                unsigned int Np)
            : m_pcf_array(pcf_array), m_nbins_r(nbins_r), m_nbins_TP(nbins_TP), m_nbins_TM(nbins_TM), m_box(box),
              m_max_r(max_r), m_max_TP(max_TP), m_max_TM(max_TM), m_dr(dr), m_dTP(dTP), m_dTM(dTM), m_lc(lc),
              m_ref_points(ref_points), m_ref_orientations(ref_orientations), m_Nref(Nref), m_points(points),
              m_orientations(orientations), m_Np(Np)
        {
        }
        void operator()( const blocked_range<size_t> &myR ) const
            {
            assert(m_ref_points);
            assert(m_points);
            assert(m_Nref > 0);
            assert(m_Np > 0);

            float dr_inv = 1.0f / m_dr;
            float maxrsq = m_max_r * m_max_r;
            float dTP_inv = 1.0f / m_dTP;
            float dTM_inv = 1.0f / m_dTM;

            // for each reference point
            for (size_t i = myR.begin(); i != myR.end(); i++)
                {
                // get the cell the point is in
                float3 ref = m_ref_points[i];
                unsigned int ref_cell = m_lc->getCell(ref);

                // loop over all neighboring cells
                const std::vector<unsigned int>& neigh_cells = m_lc->getCellNeighbors(ref_cell);
                for (unsigned int neigh_idx = 0; neigh_idx < neigh_cells.size(); neigh_idx++)
                    {
                    unsigned int neigh_cell = neigh_cells[neigh_idx];

                    // iterate over the particles in that cell
                    locality::LinkCell::iteratorcell it = m_lc->itercell(neigh_cell);
                    for (unsigned int j = it.next(); !it.atEnd(); j=it.next())
                        {
                        float3 point = m_points[j];
                        float dx = float(point.x - ref.x);
                        float dy = float(point.y - ref.y);
                        float3 delta = m_box.wrap(make_float3(dx, dy, (float)0));

                        float rsq = delta.x*delta.x + delta.y*delta.y;
                        if (rsq < 1e-6)
                            {
                            continue;
                            }
                        if (rsq < maxrsq)
                            {
                            float r = sqrtf(rsq);
                            // calculate angles
                            float dTheta1 = atan2(delta.y, delta.x);
                            float dTheta2 = atan2(-delta.y, -delta.x);
                            float T1 = dTheta1 - m_ref_orientations[i];
                            float T2 = dTheta2 - m_orientations[j];
                            float TP = T1 + T2 + m_max_TP;
                            float TM = T1 - T2 + m_max_TM;

                            // bin that point
                            float binr = r * dr_inv;
                            float binTP = floorf(TP * dTP_inv);
                            float binTM = floorf(TM * dTM_inv);
                            // fast float to int conversion with truncation
                            #ifdef __SSE2__
                            unsigned int ibinr = _mm_cvtt_ss2si(_mm_load_ss(&binr));
                            unsigned int ibinTP = _mm_cvtt_ss2si(_mm_load_ss(&binTP));
                            unsigned int ibinTM = _mm_cvtt_ss2si(_mm_load_ss(&binTM));
                            #else
                            unsigned int ibinr = (unsigned int)(binr);
                            unsigned int ibinTP = (unsigned int)(binTP);
                            unsigned int ibinTM = (unsigned int)(binTM);
                            #endif

                            if ((ibinr < m_nbins_r) && (ibinTP < m_nbins_TP) && (ibinTM < m_nbins_TM))
                                {
                                m_pcf_array[ibinr*m_nbins_TP*m_nbins_TM + ibinTP*m_nbins_TM + ibinTM]++;
                                }
                            }
                        }
                    }
                } // done looping over reference points
            }
    };

bool PMFTRPM::useCells()
    {
    float l_min = fmin(m_box.getLx(), m_box.getLy());

    if (!m_box.is2D())
        l_min = fmin(l_min, m_box.getLz());

    if (m_max_r < l_min/3.0f)
        return true;

    return false;
    }

void PMFTRPM::compute(unsigned int *pcf_array,
                        float3 *ref_points,
                        float *ref_orientations,
                        unsigned int Nref,
                        float3 *points,
                        float *orientations,
                        unsigned int Np)
    {
    if (useCells())
        {
        m_lc->computeCellList(points, Np);
        parallel_for(blocked_range<size_t>(0,Nref), ComputePMFTRPMWithCellList((atomic<unsigned int>*)pcf_array,
                                                                            m_nbins_r,
                                                                            m_nbins_TP,
                                                                            m_nbins_TM,
                                                                            m_box,
                                                                            m_max_r,
                                                                            m_max_TP,
                                                                            m_max_TM,
                                                                            m_dr,
                                                                            m_dTP,
                                                                            m_dTM,
                                                                            m_lc,
                                                                            ref_points,
                                                                            ref_orientations,
                                                                            Nref,
                                                                            points,
                                                                            orientations,
                                                                            Np));
        }
    else
        {
        parallel_for(blocked_range<size_t>(0,Nref), ComputePMFTRPMWithoutCellList((atomic<unsigned int>*)pcf_array,
                                                                               m_nbins_r,
                                                                               m_nbins_TP,
                                                                               m_nbins_TM,
                                                                               m_box,
                                                                               m_max_r,
                                                                               m_max_TP,
                                                                               m_max_TM,
                                                                               m_dr,
                                                                               m_dTP,
                                                                               m_dTM,
                                                                               ref_points,
                                                                               ref_orientations,
                                                                               Nref,
                                                                               points,
                                                                               orientations,
                                                                               Np));
        }
    }

void PMFTRPM::computePy(boost::python::numeric::array pcf_array,
                          boost::python::numeric::array ref_points,
                          boost::python::numeric::array ref_orientations,
                          boost::python::numeric::array points,
                          boost::python::numeric::array orientations)
    {
    // validate input type and rank
    num_util::check_type(pcf_array, PyArray_INT);
    num_util::check_rank(pcf_array, 3);
    num_util::check_type(ref_points, PyArray_FLOAT);
    num_util::check_rank(ref_points, 2);
    num_util::check_type(ref_orientations, PyArray_FLOAT);
    num_util::check_rank(ref_orientations, 1);
    num_util::check_type(points, PyArray_FLOAT);
    num_util::check_rank(points, 2);
    num_util::check_type(orientations, PyArray_FLOAT);
    num_util::check_rank(orientations, 1);

    // validate array dims
    num_util::check_dim(pcf_array, 0, m_nbins_r);
    num_util::check_dim(pcf_array, 1, m_nbins_TP);
    num_util::check_dim(pcf_array, 2, m_nbins_TM);

    // validate that the 2nd dimension is only 3
    num_util::check_dim(points, 1, 3);
    unsigned int Np = num_util::shape(points)[0];

    num_util::check_dim(ref_points, 1, 3);
    unsigned int Nref = num_util::shape(ref_points)[0];

    // check the size of angles to be correct
    num_util::check_dim(ref_orientations, 0, Nref);
    num_util::check_dim(orientations, 0, Np);

    // get the raw data pointers and compute the cell list
    unsigned int* pcf_array_raw = (unsigned int*) num_util::data(pcf_array);
    float3* ref_points_raw = (float3*) num_util::data(ref_points);
    float* ref_orientations_raw = (float*) num_util::data(ref_orientations);
    float3* points_raw = (float3*) num_util::data(points);
    float* orientations_raw = (float*) num_util::data(orientations);

        // compute with the GIL released
        {
        util::ScopedGILRelease gil;
        compute(pcf_array_raw,
                ref_points_raw,
                ref_orientations_raw,
                Nref,
                points_raw,
                orientations_raw,
                Np);
        }
    }

void export_PMFTRPM()
    {
    class_<PMFTRPM>("PMFTRPM", init<trajectory::Box&, float, float, float, float, float, float>())
        .def("getBox", &PMFTRPM::getBox, return_internal_reference<>())
        .def("compute", &PMFTRPM::computePy)
        .def("getR", &PMFTRPM::getRPy)
        .def("getTP", &PMFTRPM::getTPPy)
        .def("getTM", &PMFTRPM::getTMPy)
        ;
    }

}; }; // end namespace freud::pmft
