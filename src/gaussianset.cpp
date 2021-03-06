/******************************************************************************

  This source file is part of the OpenQube project.

  Copyright 2008-2010 Marcus D. Hanwell
  Copyright 2008 Albert De Fusco

  This source code is released under the New BSD License, (the "License").

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

******************************************************************************/

#include "gaussianset.h"

#ifdef WIN32
#define _USE_MATH_DEFINES
#include <math.h> // needed for M_PI
#endif

#include "cube.h"

#include <cmath>
#include <iostream>

#include <QtCore/QtConcurrentMap>
#include <QtCore/QFuture>
#include <QtCore/QFutureWatcher>
#include <QtCore/QReadWriteLock>
#include <QtCore/QDebug>

using std::vector;
using Eigen::Vector3d;
using Eigen::Vector3i;
using Eigen::MatrixXd;

namespace OpenQube
{
struct GaussianShell
{
  GaussianSet *set;  // A pointer to the GaussianSet, cannot write to member vars
  Cube *tCube;       // The target cube, used to initialise temp cubes too
  unsigned int pos;  // The index ofposition of the point to calculate the MO for
  unsigned int state;// The MO number to calculate
};

static const double BOHR_TO_ANGSTROM = 0.529177249;
static const double ANGSTROM_TO_BOHR = 1.0 / BOHR_TO_ANGSTROM;

GaussianSet::GaussianSet() : m_numMOs(0), m_numAtoms(0), m_init(false),
  m_cube(0), m_gaussianShells(0)
{
}

GaussianSet::~GaussianSet()
{
}

unsigned int GaussianSet::addAtom(const Vector3d& pos, int atomicNumber)
{
  m_init = false;
  // Add to the new data structure, delete the old soon
  m_molecule.addAtom(pos, atomicNumber);

  return m_molecule.numAtoms() - 1;
}

unsigned int GaussianSet::addBasis(unsigned int atom, orbital type)
{
  // Count the number of independent basis functions
  switch (type) {
  case S:
    m_numMOs++;
    break;
  case P:
    m_numMOs += 3;
    break;
  case SP:
    m_numMOs += 4;
    break;
  case D:
    m_numMOs += 6;
    break;
  case D5:
    m_numMOs += 5;
    break;
  case F:
    m_numMOs += 8;
    break;
  case F7:
    m_numMOs += 7;
    break;
  default:
    // Should never hit here
    ;
  }
  m_init = false;

  // Add to the new data structure, delete the old soon
  m_symmetry.push_back(type);
  m_atomIndices.push_back(atom);
  return m_symmetry.size() - 1;
}

unsigned int GaussianSet::addGTO(unsigned int, double c, double a)
{
  // Use the new data structure
  if (m_gtoIndices.size() < m_atomIndices.size()) {
    // First GTO added for this basis - add the gto index
    m_gtoIndices.push_back(m_gtoA.size());
  }
  m_gtoA.push_back(a);
  m_gtoC.push_back(c);

  return m_gtoA.size() - 1;
}

void GaussianSet::addMOs(const vector<double>& MOs)
{
  m_init = false;

  // Some programs don't output all MOs, so we take the amount of data
  // and divide by the # of AO functions
  unsigned int columns = MOs.size() / m_numMOs;
  qDebug() << " add MOs: " << m_numMOs << columns;

  m_moMatrix.resize(m_numMOs, m_numMOs);

  for (unsigned int j = 0; j < columns; ++j)
    for (unsigned int i = 0; i < m_numMOs; ++i)
      m_moMatrix.coeffRef(i, j) = MOs[i + j*m_numMOs];
}

void GaussianSet::addMO(double)
{
  m_init = false;
}

bool GaussianSet::setDensityMatrix(const Eigen::MatrixXd &m)
{
  m_density.resize(m.rows(), m.cols());
  m_density = m;
  return true;
}

bool GaussianSet::calculateCubeMO(Cube *cube, unsigned int state)
{
  // Set up the calculation and ideally use the new QtConcurrent code to
  // multithread the calculation...
  if (state < 1 || state > static_cast<unsigned int>(m_moMatrix.rows()))
    return false;

  // Must be called before calculations begin
  initCalculation();

  // Set up the points we want to calculate the density at
  m_gaussianShells = new QVector<GaussianShell>(cube->data()->size());

  for (int i = 0; i < m_gaussianShells->size(); ++i) {
    (*m_gaussianShells)[i].set = this;
    (*m_gaussianShells)[i].tCube = cube;
    (*m_gaussianShells)[i].pos = i;
    (*m_gaussianShells)[i].state = state;
  }

  // Lock the cube until we are done.
  cube->lock()->lockForWrite();

  // Set the cube type
  cube->setCubeType(Cube::MO);

  // Watch for the future
  connect(&m_watcher, SIGNAL(finished()), this, SLOT(calculationComplete()));

  // The main part of the mapped reduced function...
  m_future = QtConcurrent::map(*m_gaussianShells, GaussianSet::processPoint);
  // Connect our watcher to our future
  m_watcher.setFuture(m_future);

  return true;
}

bool GaussianSet::calculateCubeDensity(Cube *cube)
{
  if (m_density.size() == 0) {
    qDebug() << "Cannot calculate density -- density matrix not set.";
    return false;
  }

  // FIXME Still not working, committed so others could see current state.

  // Must be called before calculations begin
  initCalculation();

  // Set up the points we want to calculate the density at
  m_gaussianShells = new QVector<GaussianShell>(cube->data()->size());

  for (int i = 0; i < m_gaussianShells->size(); ++i) {
    (*m_gaussianShells)[i].set = this;
    (*m_gaussianShells)[i].tCube = cube;
    (*m_gaussianShells)[i].pos = i;
  }

  // Lock the cube until we are done.
  cube->lock()->lockForWrite();

  // Set the cube type
  cube->setCubeType(Cube::ElectronDensity);

  // Watch for the future
  connect(&m_watcher, SIGNAL(finished()), this, SLOT(calculationComplete()));

  // The main part of the mapped reduced function...
  m_future = QtConcurrent::map(*m_gaussianShells, GaussianSet::processDensity);
  // Connect our watcher to our future
  m_watcher.setFuture(m_future);

  return true;
}

BasisSet * GaussianSet::clone()
{
  GaussianSet *result = new GaussianSet();

  result->m_symmetry = this->m_symmetry;
  result->m_atomIndices = this->m_atomIndices;
  result->m_moIndices = this->m_moIndices;
  result->m_gtoIndices = this->m_gtoIndices;
  result->m_cIndices = this->m_cIndices;
  result->m_gtoA = this->m_gtoA;
  result->m_gtoC = this->m_gtoC;
  result->m_gtoCN = this->m_gtoCN;
  result->m_moMatrix = this->m_moMatrix;
  result->m_density = this->m_density;

  result->m_numMOs = this->m_numMOs;
  result->m_numAtoms = this->m_numAtoms;
  result->m_init = this->m_init;

  // Skip tmp vars
  return result;
}

void GaussianSet::calculationComplete()
{
  disconnect(&m_watcher, SIGNAL(finished()), this, SLOT(calculationComplete()));
  (*m_gaussianShells)[0].tCube->lock()->unlock();
  delete m_gaussianShells;
  m_gaussianShells = 0;
  emit finished();
}

inline bool GaussianSet::isSmall(double val)
{
  if (val > -1e-20 && val < 1e-20)
    return true;
  else
    return false;
}

void GaussianSet::initCalculation()
{
  if (m_init)
    return;
  // This currently just involves normalising all contraction coefficients
  m_numAtoms = m_molecule.numAtoms();
  m_gtoCN.clear();

  // Initialise the new data structures that are hopefully more efficient
  unsigned int indexMO = 0;
  unsigned int skip = 0; // for unimplemented shells

  m_moIndices.resize(m_symmetry.size());
  // Add a final entry to the gtoIndices
  m_gtoIndices.push_back(m_gtoA.size());
  for(unsigned int i = 0; i < m_symmetry.size(); ++i) {
    switch (m_symmetry[i]) {
    case S:
      m_moIndices[i] = indexMO++;
      m_cIndices.push_back(m_gtoCN.size());
      // Normalization of the S-type orbitals (normalization used in JMol)
      // (8 * alpha^3 / pi^3)^0.25 * exp(-alpha * r^2)
      for(unsigned j = m_gtoIndices[i]; j < m_gtoIndices[i+1]; ++j) {
        m_gtoCN.push_back(m_gtoC[j] * pow(m_gtoA[j], 0.75) * 0.71270547);
      }
      break;
    case P:
      m_moIndices[i] = indexMO;
      indexMO += 3;
      m_cIndices.push_back(m_gtoCN.size());
      // Normalization of the P-type orbitals (normalization used in JMol)
      // (128 alpha^5 / pi^3)^0.25 * [x|y|z]exp(-alpha * r^2)
      for(unsigned j = m_gtoIndices[i]; j < m_gtoIndices[i+1]; ++j) {
        m_gtoCN.push_back(m_gtoC[j] * pow(m_gtoA[j], 1.25) * 1.425410941);
        m_gtoCN.push_back(m_gtoCN.back());
        m_gtoCN.push_back(m_gtoCN.back());
      }
      break;
    case D:
      // Cartesian - 6 d components
      // Order in xx, yy, zz, xy, xz, yz
      m_moIndices[i] = indexMO;
      indexMO += 6;
      m_cIndices.push_back(m_gtoCN.size());
      // Normalization of the P-type orbitals (normalization used in JMol)
      // xx|yy|zz: (2048 alpha^7/9pi^3)^0.25 [xx|yy|zz]exp(-alpha r^2)
      // xy|xz|yz: (2048 alpha^7/pi^3)^0.25 [xy|xz|yz]exp(-alpha r^2)
      for(unsigned j = m_gtoIndices[i]; j < m_gtoIndices[i+1]; ++j) {
        m_gtoCN.push_back(m_gtoC[j] * pow(m_gtoA[j], 1.75) * 1.645922781);
        m_gtoCN.push_back(m_gtoCN.back());
        m_gtoCN.push_back(m_gtoCN.back());

        m_gtoCN.push_back(m_gtoC[j] * pow(m_gtoA[j], 1.75) * 2.850821881);
        m_gtoCN.push_back(m_gtoCN.back());
        m_gtoCN.push_back(m_gtoCN.back());
      }
      break;
    case D5:
      // Spherical - 5 d components
      // Order in d0, d+1, d-1, d+2, d-2
      // Form d(z^2-r^2), dxz, dyz, d(x^2-y^2), dxy
      m_moIndices[i] = indexMO;
      indexMO += 5;
      m_cIndices.push_back(m_gtoCN.size());
      for(unsigned j = m_gtoIndices[i]; j < m_gtoIndices[i+1]; ++j) {
        m_gtoCN.push_back(m_gtoC[j] * pow(2048 * pow(m_gtoA[j], 7.0)
                                          / (9.0 * M_PI*M_PI*M_PI), 0.25));
        m_gtoCN.push_back(m_gtoC[j] * pow(2048 * pow(m_gtoA[j], 7.0)
                                          / (M_PI*M_PI*M_PI), 0.25));
        m_gtoCN.push_back(m_gtoCN.back());
        // I think this is correct but reaally need to check...
        m_gtoCN.push_back(m_gtoC[j] * pow(128 * pow(m_gtoA[j], 7.0)
                                          / (M_PI*M_PI*M_PI), 0.25));
        m_gtoCN.push_back(m_gtoC[j] * pow(2048 * pow(m_gtoA[j], 7.0)
                                          / (M_PI*M_PI*M_PI), 0.25));
      }
      break;
    case F:
      skip = 10;
    case F7:
      skip = 7;
    case G:
      skip = 15;
    case G9:
      skip = 9;
    case H:
      skip = 21;
    case H11:
      skip = 11;
    case I:
      skip = 28;
    case I13:
      skip = 13;

      m_moIndices[i] = indexMO;
      indexMO += skip;
      m_cIndices.push_back(m_gtoCN.size());
      qDebug() << "Basis set not handled - results may be incorrect.";
      break;

    default:
      qDebug() << "Basis set not handled - results may be incorrect.";
    }
  }
  m_init = true;
}

/// This is the stuff we actually use right now - porting to new data structure
void GaussianSet::processPoint(GaussianShell &shell)
{
  GaussianSet *set = shell.set;
  unsigned int atomsSize = set->m_numAtoms;
  unsigned int basisSize = set->m_symmetry.size();
  std::vector<int> &basis = set->m_symmetry;
  vector<Vector3d> deltas;
  vector<double> dr2;
  deltas.reserve(atomsSize);
  dr2.reserve(atomsSize);

  unsigned int indexMO = shell.state-1;

  // Calculate our position
  Vector3d pos = shell.tCube->position(shell.pos) * ANGSTROM_TO_BOHR;

  // Calculate the deltas for the position
  for (unsigned int i = 0; i < atomsSize; ++i) {
    deltas.push_back(pos - set->m_molecule.atomPos(i));
    dr2.push_back(deltas[i].squaredNorm());
  }

  // Now calculate the value at this point in space
  double tmp = 0.0;
  for (unsigned int i = 0; i < basisSize; ++i) {
    switch(basis[i]) {
    case S:
      tmp += pointS(shell.set, i,
                    dr2[set->m_atomIndices[i]], indexMO);
      break;
    case P:
      tmp += pointP(shell.set, i, deltas[set->m_atomIndices[i]],
                    dr2[set->m_atomIndices[i]], indexMO);
      break;
    case D:
      tmp += pointD(shell.set, i, deltas[set->m_atomIndices[i]],
                    dr2[set->m_atomIndices[i]], indexMO);
      break;
    case D5:
      tmp += pointD5(shell.set, i, deltas[set->m_atomIndices[i]],
                     dr2[set->m_atomIndices[i]], indexMO);
      break;
    default:
      // Not handled - return a zero contribution
      ;
    }
  }
  // Set the value
  shell.tCube->setValue(shell.pos, tmp);
}

void GaussianSet::processDensity(GaussianShell &shell)
{
  GaussianSet *set = shell.set;
  unsigned int atomsSize = set->m_numAtoms;
  unsigned int basisSize = set->m_symmetry.size();
  unsigned int matrixSize = set->m_density.rows();
  std::vector<int> &basis = set->m_symmetry;
  vector<Vector3d> deltas;
  vector<double> dr2;
  deltas.reserve(atomsSize);
  dr2.reserve(atomsSize);

  // Calculate our position
  Vector3d pos = shell.tCube->position(shell.pos) * ANGSTROM_TO_BOHR;
  // Calculate the deltas for the position
  for (unsigned int i = 0; i < atomsSize; ++i) {
    deltas.push_back(pos - set->m_molecule.atomPos(i));
    dr2.push_back(deltas[i].squaredNorm());
  }

  // Calculate the basis set values at this point
  MatrixXd values(matrixSize, 1);
  for (unsigned int i = 0; i < basisSize; ++i) {
    unsigned int cAtom = set->m_atomIndices[i];
    switch(basis[i]) {
    case S:
      pointS(shell.set, dr2[cAtom], i, values);
      break;
    case P:
      pointP(shell.set, deltas[cAtom], dr2[cAtom], i, values);
      break;
    case D:
      pointD(shell.set, deltas[cAtom], dr2[cAtom], i, values);
      break;
    case D5:
      pointD5(shell.set, deltas[cAtom], dr2[cAtom], i, values);
      break;
    default:
      // Not handled - return a zero contribution
      ;
    }
  }

  // Now calculate the value of the density at this point in space
  double rho = 0.0;
  for (unsigned int i = 0; i < matrixSize; ++i) {
    // Calculate the off-diagonal parts of the matrix
    for (unsigned int j = 0; j < i; ++j) {
      rho += 2.0 * set->m_density.coeffRef(i, j)
          * (values.coeffRef(i, 0) * values.coeffRef(j, 0));
    }
    // Now calculate the matrix diagonal
    rho += set->m_density.coeffRef(i, i)
        * (values.coeffRef(i, 0) * values.coeffRef(i, 0));
  }

  // Set the value
  shell.tCube->setValue(shell.pos, rho);
}

inline double GaussianSet::pointS(GaussianSet *set, unsigned int moIndex,
                                  double dr2, unsigned int indexMO)
{
  // If the MO coefficient is very small skip it
  if (isSmall(set->m_moMatrix.coeffRef(set->m_moIndices[moIndex], indexMO))) {
    return 0.0;
  }

  // S type orbitals - the simplest of the calculations with one component
  double tmp = 0.0;
  unsigned int cIndex = set->m_cIndices[moIndex];
  for (unsigned int i = set->m_gtoIndices[moIndex];
       i < set->m_gtoIndices[moIndex+1]; ++i) {
    tmp += set->m_gtoCN[cIndex++] * exp(-set->m_gtoA[i] * dr2);
  }
  // There is one MO coefficient per S shell basis
  return tmp * set->m_moMatrix.coeffRef(set->m_moIndices[moIndex], indexMO);
}

inline double GaussianSet::pointP(GaussianSet *set, unsigned int moIndex,
                                  const Vector3d &delta,
                                  double dr2, unsigned int indexMO)
{
  // P type orbitals have three components and each component has a different
  // independent MO weighting. Many things can be cached to save time though
  unsigned int baseIndex = set->m_moIndices[moIndex];
  double x = 0.0, y = 0.0, z = 0.0;

  // Now iterate through the P type GTOs and sum their contributions
  unsigned int cIndex = set->m_cIndices[moIndex];
  for (unsigned int i = set->m_gtoIndices[moIndex];
       i < set->m_gtoIndices[moIndex+1]; ++i) {
    double tmpGTO = exp(-set->m_gtoA[i] * dr2);
    x += set->m_gtoCN[cIndex++] * delta.x() * tmpGTO;
    y += set->m_gtoCN[cIndex++] * delta.y() * tmpGTO;
    z += set->m_gtoCN[cIndex++] * delta.z() * tmpGTO;
  }

  // Calculate the prefactors for Px, Py and Pz
  double Px = set->m_moMatrix.coeffRef(baseIndex  , indexMO);
  double Py = set->m_moMatrix.coeffRef(baseIndex+1, indexMO);
  double Pz = set->m_moMatrix.coeffRef(baseIndex+2, indexMO);

  return Px*x + Py*y + Pz*z;
}

inline double GaussianSet::pointD(GaussianSet *set, unsigned int moIndex,
                                  const Vector3d &delta,
                                  double dr2, unsigned int indexMO)
{
  // D type orbitals have six components and each component has a different
  // independent MO weighting. Many things can be cached to save time though
  unsigned int baseIndex = set->m_moIndices[moIndex];
  double xx = 0.0, yy = 0.0, zz = 0.0, xy = 0.0, xz = 0.0, yz = 0.0;

  // Now iterate through the D type GTOs and sum their contributions
  unsigned int cIndex = set->m_cIndices[moIndex];
  for (unsigned int i = set->m_gtoIndices[moIndex];
       i < set->m_gtoIndices[moIndex+1]; ++i) {
    // Calculate the common factor
    double tmpGTO = exp(-set->m_gtoA[i] * dr2);
    xx += set->m_gtoCN[cIndex++] * tmpGTO; // Dxx
    yy += set->m_gtoCN[cIndex++] * tmpGTO; // Dyy
    zz += set->m_gtoCN[cIndex++] * tmpGTO; // Dzz
    xy += set->m_gtoCN[cIndex++] * tmpGTO; // Dxy
    xz += set->m_gtoCN[cIndex++] * tmpGTO; // Dxz
    yz += set->m_gtoCN[cIndex++] * tmpGTO; // Dyz
  }

  // Calculate the prefactors
  double Dxx = set->m_moMatrix.coeffRef(baseIndex  , indexMO) * delta.x()
      * delta.x();
  double Dyy = set->m_moMatrix.coeffRef(baseIndex+1, indexMO) * delta.y()
      * delta.y();
  double Dzz = set->m_moMatrix.coeffRef(baseIndex+2, indexMO) * delta.z()
      * delta.z();
  double Dxy = set->m_moMatrix.coeffRef(baseIndex+3, indexMO) * delta.x()
      * delta.y();
  double Dxz = set->m_moMatrix.coeffRef(baseIndex+4, indexMO) * delta.x()
      * delta.z();
  double Dyz = set->m_moMatrix.coeffRef(baseIndex+5, indexMO) * delta.y()
      * delta.z();
  return Dxx*xx + Dyy*yy + Dzz*zz + Dxy*xy + Dxz*xz + Dyz*yz;
}

inline double GaussianSet::pointD5(GaussianSet *set, unsigned int moIndex,
                                   const Vector3d &delta,
                                   double dr2, unsigned int indexMO)
{
  // D type orbitals have five components and each component has a different
  // MO weighting. Many things can be cached to save time
  unsigned int baseIndex = set->m_moIndices[moIndex];
  double d0 = 0.0, d1p = 0.0, d1n = 0.0, d2p = 0.0, d2n = 0.0;

  // Now iterate through the D type GTOs and sum their contributions
  unsigned int cIndex = set->m_cIndices[moIndex];
  for (unsigned int i = set->m_gtoIndices[moIndex];
       i < set->m_gtoIndices[moIndex+1]; ++i) {
    // Calculate the common factor
    double tmpGTO = exp(-set->m_gtoA[i] * dr2);
    d0  += set->m_gtoCN[cIndex++] * tmpGTO;
    d1p += set->m_gtoCN[cIndex++] * tmpGTO;
    d1n += set->m_gtoCN[cIndex++] * tmpGTO;
    d2p += set->m_gtoCN[cIndex++] * tmpGTO;
    d2n += set->m_gtoCN[cIndex++] * tmpGTO;
  }

  // Calculate the prefactors
  double xx = delta.x() * delta.x();
  double yy = delta.y() * delta.y();
  double zz = delta.z() * delta.z();
  double xy = delta.x() * delta.y();
  double xz = delta.x() * delta.z();
  double yz = delta.y() * delta.z();

  double D0  = set->m_moMatrix.coeffRef(baseIndex  , indexMO) * (zz - dr2);
  double D1p = set->m_moMatrix.coeffRef(baseIndex+1, indexMO) * xz;
  double D1n = set->m_moMatrix.coeffRef(baseIndex+2, indexMO) * yz;
  double D2p = set->m_moMatrix.coeffRef(baseIndex+3, indexMO) * (xx - yy);
  double D2n = set->m_moMatrix.coeffRef(baseIndex+4, indexMO) * xy;

  return D0*d0 + D1p*d1p + D1n*d1n + D2p*d2p + D2n*d2n;
}

inline void GaussianSet::pointS(GaussianSet *set, double dr2, int basis,
                                Eigen::MatrixXd &out)
{
  // S type orbitals - the simplest of the calculations with one component
  double tmp = 0.0;
  unsigned int cIndex = set->m_cIndices[basis];
  for (unsigned int i = set->m_gtoIndices[basis];
       i < set->m_gtoIndices[basis+1]; ++i) {
    tmp += set->m_gtoCN[cIndex++] * exp(-set->m_gtoA[i] * dr2);
  }
  out.coeffRef(set->m_moIndices[basis], 0) = tmp;
}

inline void GaussianSet::pointP(GaussianSet *set, const Vector3d &delta,
                                double dr2, int basis,
                                Eigen::MatrixXd &out)
{
  double x = 0.0, y = 0.0, z = 0.0;

  // Now iterate through the P type GTOs and sum their contributions
  unsigned int cIndex = set->m_cIndices[basis];
  for (unsigned int i = set->m_gtoIndices[basis];
       i < set->m_gtoIndices[basis+1]; ++i) {
    double tmpGTO = exp(-set->m_gtoA[i] * dr2);
    x += set->m_gtoCN[cIndex++] * tmpGTO;
    y += set->m_gtoCN[cIndex++] * tmpGTO;
    z += set->m_gtoCN[cIndex++] * tmpGTO;
  }

  // Save values to the matrix
  int baseIndex = set->m_moIndices[basis];
  out.coeffRef(baseIndex  , 0) = x * delta.x();
  out.coeffRef(baseIndex+1, 0) = y * delta.y();
  out.coeffRef(baseIndex+2, 0) = z * delta.z();
}

inline void GaussianSet::pointD(GaussianSet *set, const Eigen::Vector3d &delta,
                                double dr2, int basis,
                                Eigen::MatrixXd &out)
{
  // D type orbitals have six components and each component has a different
  // independent MO weighting. Many things can be cached to save time though
  double xx = 0.0, yy = 0.0, zz = 0.0, xy = 0.0, xz = 0.0, yz = 0.0;

  // Now iterate through the D type GTOs and sum their contributions
  unsigned int cIndex = set->m_cIndices[basis];
  for (unsigned int i = set->m_gtoIndices[basis];
       i < set->m_gtoIndices[basis+1]; ++i) {
    // Calculate the common factor
    double tmpGTO = exp(-set->m_gtoA[i] * dr2);
    xx += set->m_gtoCN[cIndex++] * tmpGTO; // Dxx
    yy += set->m_gtoCN[cIndex++] * tmpGTO; // Dyy
    zz += set->m_gtoCN[cIndex++] * tmpGTO; // Dzz
    xy += set->m_gtoCN[cIndex++] * tmpGTO; // Dxy
    xz += set->m_gtoCN[cIndex++] * tmpGTO; // Dxz
    yz += set->m_gtoCN[cIndex++] * tmpGTO; // Dyz
  }

  // Save values to the matrix
  int baseIndex = set->m_moIndices[basis];
  out.coeffRef(baseIndex  , 0) = delta.x() * delta.x() * xx;
  out.coeffRef(baseIndex+1, 0) = delta.y() * delta.y() * yy;
  out.coeffRef(baseIndex+2, 0) = delta.z() * delta.z() * zz;
  out.coeffRef(baseIndex+3, 0) = delta.x() * delta.y() * xy;
  out.coeffRef(baseIndex+4, 0) = delta.x() * delta.z() * xz;
  out.coeffRef(baseIndex+5, 0) = delta.y() * delta.z() * yz;
}

inline void GaussianSet::pointD5(GaussianSet *set, const Eigen::Vector3d &delta,
                                 double dr2, int basis,
                                 Eigen::MatrixXd &out)
{
  // D type orbitals have six components and each component has a different
  // independent MO weighting. Many things can be cached to save time though
  double d0 = 0.0, d1p = 0.0, d1n = 0.0, d2p = 0.0, d2n = 0.0;

  // Now iterate through the D type GTOs and sum their contributions
  unsigned int cIndex = set->m_cIndices[basis];
  for (unsigned int i = set->m_gtoIndices[basis];
       i < set->m_gtoIndices[basis+1]; ++i) {
    // Calculate the common factor
    double tmpGTO = exp(-set->m_gtoA[i] * dr2);
    d0  += set->m_gtoCN[cIndex++] * tmpGTO;
    d1p += set->m_gtoCN[cIndex++] * tmpGTO;
    d1n += set->m_gtoCN[cIndex++] * tmpGTO;
    d2p += set->m_gtoCN[cIndex++] * tmpGTO;
    d2n += set->m_gtoCN[cIndex++] * tmpGTO;
  }

  // Calculate the prefactors
  double xx = delta.x() * delta.x();
  double yy = delta.y() * delta.y();
  double zz = delta.z() * delta.z();
  double xy = delta.x() * delta.y();
  double xz = delta.x() * delta.z();
  double yz = delta.y() * delta.z();

  // Save values to the matrix
  int baseIndex = set->m_moIndices[basis];
  out.coeffRef(baseIndex  , 0) = (zz - dr2) * d0;
  out.coeffRef(baseIndex+1, 0) = xz * d1p;
  out.coeffRef(baseIndex+2, 0) = yz * d1n;
  out.coeffRef(baseIndex+3, 0) = (xx - yy) * d2p;
  out.coeffRef(baseIndex+4, 0) = xy * d2n;
}

unsigned int GaussianSet::numMOs()
{
  // Return the total number of MOs
  return m_moMatrix.rows();
}

void GaussianSet::outputAll()
{
  // Can be called to print out a summary of the basis set as read in
  qDebug() << "\nGaussian Basis Set\nNumber of atoms:" << m_numAtoms;

  initCalculation();

  if (!isValid()) {
    qDebug() << "Basis set is marked as invalid.";
    return;
  }

  for (uint i = 0; i < m_symmetry.size(); ++i) {
    qDebug() << i
             << "\tAtom Index:" << m_atomIndices[i]
             << "\tSymmetry:" << m_symmetry[i]
             << "\tMO Index:" << m_moIndices[i]
             << "\tGTO Index:" << m_gtoIndices[i];
  }
  qDebug() << "Symmetry:" << m_symmetry.size()
           << "\tgtoIndices:" << m_gtoIndices.size()
           << "\tLast gtoIndex:" << m_gtoIndices[m_symmetry.size()]
           << "\ngto size:" << m_gtoA.size() << m_gtoC.size() << m_gtoCN.size();
  for (uint i = 0; i < m_symmetry.size(); ++i) {
    switch(m_symmetry[i]) {
    case S:
      qDebug() << "Shell" << i << "\tS\n  MO 1\t"
               << m_moMatrix(0, m_moIndices[i])
               << m_moMatrix(m_moIndices[i], 0);
      break;
    case P:
      qDebug() << "Shell" << i << "\tP\n  MO 1\t"
               << m_moMatrix(0, m_moIndices[i])
               << "\t" << m_moMatrix(0, m_moIndices[i] + 1)
               << "\t" << m_moMatrix(0, m_moIndices[i] + 2);
      break;
    case D:
      qDebug() << "Shell" << i << "\tD\n  MO 1\t"
               << m_moMatrix(0, m_moIndices[i])
               << "\t" << m_moMatrix(0, m_moIndices[i] + 1)
               << "\t" << m_moMatrix(0, m_moIndices[i] + 2)
               << "\t" << m_moMatrix(0, m_moIndices[i] + 3)
               << "\t" << m_moMatrix(0, m_moIndices[i] + 4)
               << "\t" << m_moMatrix(0, m_moIndices[i] + 5);
      break;
    case D5:
      qDebug() << "Shell" << i << "\tD5\n  MO 1\t"
               << m_moMatrix(0, m_moIndices[i])
               << "\t" << m_moMatrix(0, m_moIndices[i] + 1)
               << "\t" << m_moMatrix(0, m_moIndices[i] + 2)
               << "\t" << m_moMatrix(0, m_moIndices[i] + 3)
               << "\t" << m_moMatrix(0, m_moIndices[i] + 4);
      break;
    case F:
      std::cout << "Shell " << i << "\tF\n  MO 1";
      for (short j = 0; j < 10; ++j)
        std::cout << "\t" << m_moMatrix(0, m_moIndices[i] + j);
      std::cout << std::endl;
      break;
    case F7:
      std::cout << "Shell " << i << "\tF7\n  MO 1";
      for (short j = 0; j < 7; ++j)
        std::cout << "\t" << m_moMatrix(0, m_moIndices[i] + j);
      std::cout << std::endl;
      break;
    default:
      qDebug() << "Error: unhandled type...";
    }
    unsigned int cIndex = m_gtoIndices[i];
    for (uint j = m_gtoIndices[i]; j < m_gtoIndices[i+1]; ++j) {
      if (j >= m_gtoA.size()) {
        qDebug() << "Error, j is too large!" << j << m_gtoA.size();
        continue;
      }
      qDebug() << cIndex
               << "\tc:" << m_gtoC[cIndex]
               << "\ta:" << m_gtoA[cIndex];
      ++cIndex;
    }
  }
  qDebug() << "\nEnd of orbital data...\n";
}

}
