/*********************************************************************
 *
 * Authors: Vincenzo Ciaschini - Vincenzo.Ciaschini@cnaf.infn.it 
 *
 * Copyright (c) 2002, 2003 INFN-CNAF on behalf of the EU DataGrid.
 * For license conditions see LICENSE file or
 * http://www.edg.org/license.html
 *
 * Parts of this code may be based upon or even include verbatim pieces,
 * originally written by other people, in which case the original header
 * follows.
 *
 *********************************************************************/

#include "config.h"

#include <dbwrap.h>

bool operator==(const gattrib &lhs, 
                const gattrib &rhs)
{
  return ((lhs.name == rhs.name) && (lhs.qualifier == rhs.qualifier) &&
          (lhs.value == rhs.value));
}

bool operator<(const gattrib &lhs,
               const gattrib &rhs)
{
  return lhs.str() < rhs.str();

//   const char *s1 = lhs.str().c_str();
//   const char *s2 = rhs.str().c_str();

//   return strcmp(s1, s2) < 0;
}
