/******************************************************************************

  This source file is part of the OpenQube project.

  Copyright 2008-2010 Marcus D. Hanwell

  This source code is released under the New BSD License, (the "License").

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

******************************************************************************/

#include "basissetloader.h"

#include "gaussianset.h"
#include "slaterset.h"
#include "gamessukout.h"
#include "gaussianfchk.h"
#include "mopacaux.h"
#include "molden.h"
#include "gamessus.h"
#include "cube.h"

#include <QDir>
#include <QFileInfo>
#include <QStringList>

namespace openqube {

QString BasisSetLoader::MatchBasisSet(const QString& filename)
{
  QString matchedFile;
  if (filename.isEmpty())
    return matchedFile;

  QFileInfo parentInfo(filename);
  // Look for files with the same basename, but different extensions
  QDir parentDir = parentInfo.dir();
  QStringList nameFilters;
  nameFilters << parentInfo.baseName() + ".*";

  QStringList matchingFiles = parentDir.entryList(nameFilters,
                                                  QDir::Readable | QDir::Files);
  matchingFiles.prepend(parentInfo.fileName());

  // Iterate through the matches and see if we find a suitable file
  foreach(const QString &fileName, matchingFiles) {
    QString fullFileName = parentInfo.path() + '/' + fileName;
    QFileInfo info(fullFileName);
    QString completeSuffix = info.completeSuffix();

    if (completeSuffix.contains("fchk", Qt::CaseInsensitive)
        || completeSuffix.contains("fch", Qt::CaseInsensitive)
        || completeSuffix.contains("fck", Qt::CaseInsensitive)) {
      return fullFileName;
    }
    else if (completeSuffix.contains("gukout", Qt::CaseInsensitive)) {
      return fullFileName;
    }
    else if (completeSuffix.contains("aux", Qt::CaseInsensitive)) {
      return fullFileName;
    }
    else if (completeSuffix.contains("molden", Qt::CaseInsensitive)
        || completeSuffix.contains("mold", Qt::CaseInsensitive)
        || completeSuffix.contains("molf", Qt::CaseInsensitive)) {
     return fullFileName;
    }
  }
  return matchedFile;
}

BasisSet * BasisSetLoader::LoadBasisSet(const QString& filename)
{
  // Here we assume that the file name is correct, and attempt to load it.
  QFileInfo info(filename);
  QString completeSuffix = info.completeSuffix();
  if (completeSuffix.contains("fchk", Qt::CaseInsensitive)
      || completeSuffix.contains("fch", Qt::CaseInsensitive)
      || completeSuffix.contains("fck", Qt::CaseInsensitive)) {
    GaussianSet *gaussian = new GaussianSet;
    GaussianFchk fchk(filename, gaussian);

    return gaussian;
  }
  else if (completeSuffix.contains("gukout", Qt::CaseInsensitive)) {
    GaussianSet *gaussian = new GaussianSet;
    GamessukOut gukout(filename, gaussian);
    return gaussian;
  }
  else if (completeSuffix.contains("aux", Qt::CaseInsensitive)) {
    SlaterSet *slater = new SlaterSet;
    MopacAux aux(filename, slater);
    return slater;
  }
  else if (completeSuffix.contains("molden", Qt::CaseInsensitive)
      || completeSuffix.contains("mold", Qt::CaseInsensitive)
      || completeSuffix.contains("molf", Qt::CaseInsensitive)) {
   GaussianSet *gaussian = new GaussianSet;
   MoldenFile mold(filename, gaussian);
   return gaussian;
  }

  return 0;
}

} // End namespace