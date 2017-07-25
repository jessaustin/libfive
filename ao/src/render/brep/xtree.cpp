#include <numeric>
#include <functional>
#include <limits>

#include "ao/render/brep/xtree.hpp"

namespace Kernel {

//  Here's our cutoff value (with a value set in the header)
template <unsigned N> constexpr double XTree<N>::EIGENVALUE_CUTOFF;

//  Used for compile-time checking of array bounds in vertex finding
constexpr static unsigned _pow(unsigned x, unsigned y)
{ return y ? x * _pow(x, y - 1) : 1; }

template <unsigned N>
XTree<N>::XTree(Evaluator* eval, Region<N> region)
    : region(region)
{
    // Do a preliminary evaluation to prune the tree
    auto i = eval->eval(region.lower3(), region.upper3());

    eval->push();
    if (Interval::isFilled(i))
    {
        type = Interval::FILLED;
    }
    else if (Interval::isEmpty(i))
    {
        type = Interval::EMPTY;
    }
    // If the cell wasn't empty or filled, attempt to subdivide and recurse
    else
    {
        bool all_empty = true;
        bool all_full  = true;

        // Recurse until volume is too small
        if (region.volume() > 0.001)
        {
            auto rs = region.subdivide();
            for (uint8_t i=0; i < children.size(); ++i)
            {
                // Populate child recursively
                children[i].reset(new XTree<N>(eval, rs[i]));

                // Grab corner values from children
                corners[i] = children[i]->corners[i];

                all_empty &= children[i]->type == Interval::EMPTY;
                all_full  &= children[i]->type == Interval::FILLED;
            }
            eval->pop();
        }
        // Terminate recursion here
        else
        {
            // Pack corners into evaluator
            for (uint8_t i=0; i < children.size(); ++i)
            {
                Eigen::Vector3f pos;
                pos << cornerPos(i).template cast<float>(), region.perp;
                eval->set(pos, i);
            }

            // Evaluate the region's corners and unpack from evaluator
            const float* fs = eval->values(children.size());
            for (uint8_t i=0; i < children.size(); ++i)
            {
                corners[i] = (fs[i] < 0) ? Interval::FILLED : Interval::EMPTY;
                all_full  &=  corners[i];
                all_empty &= !corners[i];
            }
        }
        type = all_empty ? Interval::EMPTY
             : all_full  ? Interval::FILLED : Interval::AMBIGUOUS;
    }
    eval->pop();

    // If this cell is unambiguous, then fill its corners with values
    if (type == Interval::FILLED || type == Interval::EMPTY)
    {
        std::fill(corners.begin(), corners.end(), type);
        manifold = true;
    }


    // Branch checking and simplifications
    if (isBranch())
    {
        // Store this tree's depth as a function of its children
        level = std::accumulate(children.begin(), children.end(), (unsigned)0,
            [](const unsigned& a, const std::unique_ptr<XTree<N>>& b)
            { return std::max(a, b->level);} ) + 1;

        // If all children are non-branches, then we could collapse
        if (std::all_of(children.begin(), children.end(),
                        [](const std::unique_ptr<XTree<N>>& o)
                        { return !o->isBranch(); }))
        {
            //  This conditional implements the three checks described in
            //  [Ju et al, 2002] in the section titled
            //      "Simplification with topology safety"
            manifold = cornersAreManifold() &&
                std::all_of(children.begin(), children.end(),
                        [](const std::unique_ptr<XTree<N>>& o)
                        { return o->manifold; }) &&
                leafsAreManifold();

            // Attempt to collapse this tree by positioning the vertex
            // in the summed QEF and checking to see if the error is small
            if (manifold)
            {
                // Populate the feature rank as the maximum of all children
                // feature ranks (as seen in DC: The Secret Sauce)
                rank = std::accumulate(
                        children.begin(), children.end(), (unsigned)0,
                        [](unsigned a, const std::unique_ptr<XTree<N>>& b)
                            { return std::max(a, b->rank);} );

                // Accumulate the mass point and QEF matrices
                for (const auto& c : children)
                {
                    if (c->rank == rank)
                    {
                        _mass_point += c->_mass_point;
                    }
                    AtA += c->AtA;
                    AtB += c->AtB;
                    BtB += c->BtB;
                }

                // If the vertex error is below a threshold, then convert
                // into a leaf by erasing all of the child branches
                if (findVertex() < 1e-8)
                {
                    std::for_each(children.begin(), children.end(),
                        [](std::unique_ptr<XTree<N>>& o) { o.reset(); });
                }
            }
        }
    }
    else if (type == Interval::AMBIGUOUS)
    {
        // Figure out if the leaf is manifold
        manifold = cornersAreManifold();

        // Populate mass point here, as we use it in both the non-manifold
        // case (where it becomes the cell's vertex) and the manifold case
        // (where we minimize the QEF towards it)
        for (auto e : edges())
        {
            if (cornerState(e.first) != cornerState(e.second))
            {
                auto inside = (cornerState(e.first) == Interval::FILLED)
                    ? cornerPos(e.first) : cornerPos(e.second);
                auto outside = (cornerState(e.first) == Interval::FILLED)
                    ? cornerPos(e.second) : cornerPos(e.first);

                // We do an N-fold reduction at each stage
                constexpr int _N = 4;
                constexpr int SEARCH_COUNT = 16;
                constexpr int NUM = (1 << _N);
                constexpr int ITER = SEARCH_COUNT / _N;

                // Binary search for intersection
                for (int i=0; i < ITER; ++i)
                {
                    // Load search points into evaluator
                    Eigen::Array<double, N, 1> ps[NUM];
                    for (int j=0; j < NUM; ++j)
                    {
                        double frac = j / (N - 1.0);
                        ps[j] = (inside * (1 - frac)) + (outside * frac);
                        Eigen::Vector3f pos;
                        pos << ps[j].template cast<float>(), region.perp;
                        eval->setRaw(pos, j);
                    }

                    // Evaluate, then search for the first inside point
                    // and adjust inside / outside to their new positions
                    auto out = eval->values(NUM);
                    for (int j=0; j < NUM; ++j)
                    {
                        if (out[j] >= 0)
                        {
                            inside = ps[j - 1];
                            outside = ps[j];
                            break;
                        }
                    }
                }

                // Accumulate this intersection in the mass point
                Eigen::Matrix<double, N + 1, 1> mp;
                mp << inside, 1;
                _mass_point += mp;
            }
        }

        // If this leaf cell is manifold, then find its vertex
        // Here, we diverge from standard DC, using the sampling strategy
        // from DMC (with regularly spaced samples on a grid), then solving
        // for the constrained minimizer with w = 0 (as described in the
        // "sliver elimination" section of the DMC paper).
        if (manifold)
        {
            constexpr unsigned R = 4;
            constexpr unsigned num = _pow(R, N);
            static_assert(num < Result::N, "Bad resolution");

            // Pre-compute per-axis grid positions
            Eigen::Array<double, R, N> pts;
            for (unsigned i=0; i < R; ++i)
            {
                const double frac = i / (R - 1.0f);
                pts.row(i) = region.lower.template cast<double>() * (1 - frac) +
                             region.upper.template cast<double>() * frac;
            }

            // Load all sample points into the evaluator
            Eigen::Array<double, num, N> positions;
            for (unsigned i=0; i < num; ++i)
            {
                // Unpack from grid positions into the position vector
                for (unsigned j=0; j < N; ++j)
                {
                    positions(i, j) = pts((i % _pow(R, j + 1)) / _pow(R, j), j);
                }

                // The evaluator works in 3-space,
                // regardless of the XTree's dimensionality
                Eigen::Vector3f pos;
                pos << positions.row(i).transpose().template cast<float>(),
                       region.perp;
                eval->set(pos, i);
            }

            // Get derivatives!
            auto ds = eval->derivs(num);

            //  The A matrix is of the form
            //  [n1x, n1y, n1z]
            //  [n2x, n2y, n2z]
            //  [n3x, n3y, n3z]
            //  ...
            //  (with one row for each sampled point's normal)
            Eigen::Matrix<double, num, N> A;

            //  The b matrix is of the form
            //  [p1 . n1 - w1]
            //  [p2 . n2 - w2]
            //  [p3 . n3 - w3]
            //  ...
            //  (with one row for each sampled point)
            Eigen::Matrix<double, num, 1> b;

            // Load samples into the QEF arrays
            for (unsigned i=0; i < num; ++i)
            {
                // Load this row of A matrix, with a special case for
                // situations with NaN derivatives
                auto derivs = Eigen::Array3d(ds.dx[i], ds.dy[i], ds.dz[i]);
                if (std::isnan(ds.dx[i]) || std::isnan(ds.dy[i]) ||
                    std::isnan(ds.dz[i]))
                {
                    derivs << 0, 0, 0;
                }
                else
                {
                    derivs /= derivs.matrix().norm();
                }
                A.row(i) << derivs.head<N>().transpose();
                b(i) = A.row(i).dot(positions.row(i).matrix()) - ds.v[i];
            }

            // Save compact QEF matrices
            auto At = A.transpose();
            AtA = At * A;
            AtB = At * b;
            BtB = b.transpose() * b;

            // Use eigenvalues to find rank, then re-use the solver
            // to find vertex position
            Eigen::EigenSolver<Eigen::Matrix<double, N, N>> es(AtA);
            auto eigenvalues = es.eigenvalues().real();

            // Count non-singular Eigenvalues to determine rank
            rank = (eigenvalues.array().abs() >= EIGENVALUE_CUTOFF).count();

            // Re-use the solver to find the vertex position, ignoring the
            // error result (because this is the bottom of the recursion)
            findVertex(es);
        }
        else
        {
            // For non-manifold leaf nodes, put the vertex at the mass point.
            // As described in "Dual Contouring: The Secret Sauce", this improves
            // mesh quality.
            vert = massPoint();
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

template <unsigned N>
uint8_t XTree<N>::cornerMask() const
{
    uint8_t mask = 0;
    for (unsigned i=0; i < children.size(); ++i)
    {
        if (cornerState(i) == Interval::FILLED)
        {
            mask |= 1 << i;
        }
    }
    return mask;
}

////////////////////////////////////////////////////////////////////////////////

template <unsigned N>
double XTree<N>::findVertex()
{
    Eigen::EigenSolver<Eigen::Matrix<double, N, N>> es(AtA);
    return findVertex(es);
}

template <unsigned N>
double XTree<N>::findVertex(
        Eigen::EigenSolver<Eigen::Matrix<double, N, N>>& es)
{
    // We need to find the pseudo-inverse of AtA.
    auto eigenvalues = es.eigenvalues().real();

    // Truncate near-singular eigenvalues in the SVD's diagonal matrix
    Eigen::Matrix<double, N, N> D = Eigen::Matrix<double, N, N>::Zero();
    for (unsigned i=0; i < N; ++i)
    {
        D.diagonal()[i] = (std::abs(eigenvalues[i]) < EIGENVALUE_CUTOFF)
            ? 0 : (1 / eigenvalues[i]);
    }

    // Sanity-checking that rank matches eigenvalue count
    if (!isBranch())
    {
        assert(D.diagonal().count() == rank);
    }

    // SVD matrices
    auto U = es.eigenvectors().real(); // = V

    // Pseudo-inverse of A
    auto AtAp = U * D * U.transpose();

    // Solve for vertex (minimizing distance to center)
    auto center = massPoint();
    vert = AtAp * (AtB - (AtA * center)) + center;

    // Return the QEF error
    return (vert.matrix().transpose() * AtA * vert.matrix() - 2*vert.matrix().transpose() * AtB)[0] + BtB;
}

////////////////////////////////////////////////////////////////////////////////

template <unsigned N>
Eigen::Vector3d XTree<N>::vert3() const
{
    Eigen::Vector3d out;
    out << vert, region.perp.template cast<double>();
    return out;
}

template <unsigned N>
Eigen::Matrix<double, N, 1> XTree<N>::massPoint() const
{
    return _mass_point.template head<N>() / _mass_point(N);
}

////////////////////////////////////////////////////////////////////////////////

// Explicit initialization of templates
template class XTree<2>;
template class XTree<3>;

}   // namespace Kernel
