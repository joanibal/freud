// Copyright (c) 2010-2019 The Regents of the University of Michigan
// This file is from the freud project, released under the BSD 3-Clause License.

#ifndef LINKCELL_H
#define LINKCELL_H

#include <memory>
#include <tbb/concurrent_hash_map.h>
#include <unordered_set>
#include <vector>

#include "Box.h"
#include "NeighborList.h"
#include "NeighborQuery.h"

/*! \file LinkCell.h
    \brief Build a cell list from a set of points.
*/

namespace freud { namespace locality {

/*! \internal
    \brief Signifies the end of the linked list
*/
const unsigned int LINK_CELL_TERMINATOR = 0xffffffff;

//! Iterates over particles in a link cell list generated by LinkCell
/*! The link-cell structure is not trivial to iterate over. This helper class
 *  makes that easier both in C++ and provides a Python compatible interface
 *  for direct usage there. An IteratorLinkCell is given the bare essentials
 *  it needs to iterate over a given cell, the cell list, the number of
 *  particles, number of cells and the cell to iterate over. Call next() to
 *  get the index of the next particle in the cell, atEnd() will return true
 *  if you are at the end. In C++, next() will crash the code if you attempt
 *  to iterate past the end (no bounds checking for performance). When called
 *  from Python, a different version of next() is used that will throw
 *  StopIteration at the end.
 *
 *  A loop over all of the particles in a cell can be accomplished with the
 *   following code in C++.
 * \code
 * LinkCell::iteratorcell it = lc.itercell(cell);
 * for (unsigned int i = it.next(); !it.atEnd(); i=it.next())
 *     {
 *     // do something with particle i
 *     }
 * \endcode
 *
 * \note Behavior is undefined if an IteratorLinkCell is accessed after the
 * parent LinkCell is destroyed.
 */
class IteratorLinkCell
{
public:
    IteratorLinkCell() : m_Np(0), m_Nc(0), m_cur_idx(LINK_CELL_TERMINATOR), m_cell(0) {}

    IteratorLinkCell(const util::ManagedArray<unsigned int> cell_list, unsigned int Np, unsigned int Nc,
                     unsigned int cell)
        : m_cell_list(cell_list), m_Np(Np), m_Nc(Nc)
    {
        m_cell = cell;
        m_cur_idx = m_Np + cell;
    }

    //! Copy the position of rhs into this object
    void copy(const IteratorLinkCell& rhs);

    //! Test if the iteration over the cell is complete
    bool atEnd();

    //! Get the next particle index in the list
    unsigned int next();

    //! Get the first particle index in the list
    unsigned int begin();

private:
    util::ManagedArray<unsigned int> m_cell_list; //!< The cell list
    unsigned int m_Np;               //!< Number of particles in the cell list
    unsigned int m_Nc;               //!< Number of cells in the cell list
    unsigned int m_cur_idx;          //!< Current index
    unsigned int m_cell;             //!< Cell being considered
};

//! Iterates over sets of shells in a cell list
/*! This class provides a convenient way to iterate over distinct
 *  shells in a cell list structure. For a range of N, these are the
 *  faces, edges, and corners of a cube of edge length 2*N + 1 cells
 *  large. While IteratorLinkCell provides a way to iterate over
 *  neighbors given a cell, IteratorCellShell provides a way to find
 *  which cell offsets should be applied to find all the neighbors of
 *  a particular reference shell within a distance some number of
 *  cells away.

 *  \code
 *  // Grab neighbor cell offsets within the 3x3x3 typical search distance
 *  for(IteratorCellShell iter(0); iter != IteratorCellShell(2); ++iter)
 *  {
 *      // still need to apply modulo operation for dimensions of the cell list
 *      const vec3<int> offset(*iter);
 *  }
 *  \endcode
 */
class IteratorCellShell
{
public:
    IteratorCellShell(unsigned int range = 0, bool is2D = false) : m_is2D(is2D)
    {
        reset(range);
    }

    //! Advance iterator to the next cell to check.
    void operator++();

    //! Get the integral coordinates of the current cell.
    vec3<int> operator*()
    {
        return vec3<int>(m_current_x, m_current_y, m_current_z);
    }

    bool operator==(const IteratorCellShell& other)
    {
        return m_range == other.m_range && m_current_x == other.m_current_x
            && m_current_y == other.m_current_y && m_current_z == other.m_current_z
            && m_stage == other.m_stage && m_is2D == other.m_is2D;
    }

    bool operator!=(const IteratorCellShell& other)
    {
        return !(*this == other);
    }

    int getRange() const
    {
        return m_range;
    }

    int m_range;  //!< Find cells this many cells away
    char m_stage; //!< stage of the computation (which face is being iterated over)

private:
    void reset(unsigned int range);

    int m_current_x; //!< Current position in x
    int m_current_y; //!< Current position in y
    int m_current_z; //!< Current position in z
    bool m_is2D;     //!< true if the cell list is 2D
};

//! Computes a cell id for each particle and a link cell data structure for iterating through it
/*! For simplicity in only needing a small number of arrays, the link cell
 *  algorithm is used to generate and store the cell list data for particles.

 *  Cells are given a nominal minimum width \a cell_width. Each dimension of
 *  the box is split into an integer number of cells no smaller than
 *  \a cell_width wide in that dimension. The actual number of cells along
 *  each dimension is stored in an Index3D which is also used to compute the
 *  cell index from (i,j,k).

 *  The cell coordinate (i,j,k) itself is computed like so:
 *  \code
 *  i = floorf((x + Lx/2) / w) % Nw
 *  \endcode
 *  and so on for j, k (y, z). Call getCellCoord() to do this computation for
 *  an arbitrary point.

 *  <b>Data structures:</b><br>
 *  The internal data structure used in LinkCell is a linked list of particle
 *  indices. See IteratorLinkCell for information on how to iterate through these.

 *  <b>2D:</b><br>
 *  LinkCell properly handles 2D boxes. When a 2D box is handed to LinkCell,
 *  it creates an m x n x 1 cell list and neighbor cells are only listed in
 *  the plane. As with everything else in freud, 2D points must be passed in
 *  as 3 component vectors x,y,0. Failing to set 0 in the third component will
 *  lead to undefined behavior.
 */
class LinkCell : public NeighborQuery
{
public:
    //! iterator to iterate over particles in the cell
    typedef IteratorLinkCell iteratorcell;

    //! Null Constructor
    LinkCell();

    //! Constructor
    LinkCell(const box::Box& box, const vec3<float>* points, unsigned int n_points, float cell_width=0);

    //! Compute LinkCell dimensions
    const vec3<unsigned int> computeDimensions(const box::Box& box, float cell_width) const;

    //! Compute cell id from cell coordinates
    unsigned int getCellIndex(const vec3<int> cellCoord) const;

    //! Get the number of cells
    unsigned int getNumCells() const
    {
        return m_size;
    }

    //! Get the cell width
    float getCellWidth() const
    {
        return m_cell_width;
    }

    //! Compute the cell id for a given position
    unsigned int getCell(const vec3<float>& p) const
    {
        vec3<unsigned int> c = getCellCoord(p);
        return coordToIndex(c.x, c.y, c.z);
    }

    //! Convert xyz coordinates to a linear index.
    vec3<unsigned int> indexToCoord(unsigned int x) const;

    //! Convert xyz coordinates to a linear index.
    unsigned int coordToIndex(int x, int y, int z) const;

    //! Compute cell coordinates for a given position
    vec3<unsigned int> getCellCoord(const vec3<float> p) const;

    //! Iterate over particles in a cell
    iteratorcell itercell(unsigned int cell) const
    {
        return iteratorcell(m_cell_list, m_n_points, getNumCells(), cell);
    }

    //! Get a list of neighbors to a cell
    const std::vector<unsigned int>& getCellNeighbors(unsigned int cell) const;

    //! Compute the cell list
    void computeCellList(const vec3<float>* points, unsigned int n_points);

    //! Implementation of per-particle query for LinkCell (see NeighborQuery.h for documentation).
    /*! \param query_point The point to find neighbors for.
     *  \param n_query_points The number of query points.
     *  \param qargs The query arguments that should be used to find neighbors.
     */
    virtual std::shared_ptr<NeighborQueryPerPointIterator> querySingle(const vec3<float> query_point, unsigned int query_point_idx,
                                                                 QueryArgs args) const;

private:
    //! Rounding helper function.
    static unsigned int roundDown(unsigned int v, unsigned int m);

    //! Helper function to compute cell neighbors
    const std::vector<unsigned int>& computeCellNeighbors(unsigned int cell) const;

    unsigned int m_n_points;      //!< Number of particles last placed into the cell list
    unsigned int m_Nc;            //!< Number of cells last used
    float m_cell_width;           //!< Minimum necessary cell width cutoff
    vec3<unsigned int> m_celldim; //!< Cell dimensions
    unsigned int m_size; //!< The size of cell list.

    util::ManagedArray<unsigned int> m_cell_list; //!< The cell list last computed
    typedef tbb::concurrent_hash_map<unsigned int, std::vector<unsigned int>> CellNeighbors;
    mutable CellNeighbors m_cell_neighbors; //!< Hash map of cell neighbors for each cell
};

//! Parent class of LinkCell iterators that knows how to traverse general cell-linked list structures.
class LinkCellIterator : public NeighborQueryPerPointIterator
{
public:
    //! Constructor
    /*! The initial state is to search shell 0, the current cell. We then
     *  iterate outwards from there.
     */
    LinkCellIterator(const LinkCell* neighbor_query, const vec3<float> query_point, unsigned int query_point_idx,
                     float r_max, float r_min, bool exclude_ii)
        : NeighborQueryPerPointIterator(neighbor_query, query_point, query_point_idx, r_max, r_min, exclude_ii), m_linkcell(neighbor_query),
          m_neigh_cell_iter(0, neighbor_query->getBox().is2D()),
          m_cell_iter(m_linkcell->itercell(m_linkcell->getCell(m_query_point)))
    {}

    //! Empty Destructor
    virtual ~LinkCellIterator() {}

protected:
    const LinkCell* m_linkcell; //!< Link to the LinkCell object
    IteratorCellShell
        m_neigh_cell_iter; //!< The shell iterator indicating how far out we're currently searching.
    LinkCell::iteratorcell
        m_cell_iter; //!< The cell iterator indicating which cell we're currently searching.
    std::unordered_set<unsigned int>
        m_searched_cells; //!< Set of cells that have already been searched by the cell shell iterator.
};

//! Iterator that gets specified numbers of nearest neighbors from LinkCell tree structures.
class LinkCellQueryIterator : public LinkCellIterator
{
public:
    //! Constructor
    LinkCellQueryIterator(const LinkCell* neighbor_query, const vec3<float> query_point, unsigned int query_point_idx,
                          unsigned int num_neighbors, float r_max, float r_min, bool exclude_ii)
        : LinkCellIterator(neighbor_query, query_point, query_point_idx, r_max, r_min, exclude_ii), m_count(0), m_num_neighbors(num_neighbors)
    {}

    //! Empty Destructor
    virtual ~LinkCellQueryIterator() {}

    //! Get the next element.
    virtual NeighborBond next();

protected:
    unsigned int m_count;                           //!< Number of neighbors returned for the current point.
    unsigned int m_num_neighbors;                               //!< Number of nearest neighbors to find
    std::vector<NeighborBond> m_current_neighbors; //!< The current set of found neighbors.
};

//! Iterator that gets neighbors in a ball of size r using LinkCell tree structures.
class LinkCellQueryBallIterator : public LinkCellIterator
{
public:
    //! Constructor
    LinkCellQueryBallIterator(const LinkCell* neighbor_query, const vec3<float> query_point, unsigned int query_point_idx,
                              float r_max, float r_min, bool exclude_ii)
        : LinkCellIterator(neighbor_query, query_point, query_point_idx, r_max, r_min, exclude_ii)
    {
        // Upon querying, if the search radius is equal to the cell width, we
        // can guarantee that we don't need to search the cell shell past the
        // query radius. For simplicity, we store this value as an integer.
        if (m_r_max == neighbor_query->getCellWidth())
        {
            m_extra_search_width = 0;
        }
        else
        {
            m_extra_search_width = 1;
        }
    }

    //! Empty Destructor
    virtual ~LinkCellQueryBallIterator() {}

    //! Get the next element.
    virtual NeighborBond next();

protected:
    int m_extra_search_width; //!< The extra shell distance to search, always 0 or 1.
};
}; }; // end namespace freud::locality

#endif // LINKCELL_H
