/* This file is part of the KDE libraries
   Copyright (C) 2002,2003 Hamish Rodda <rodda@kde.org>
   Copyright (C) 2003      Anakim Border <aborder@sources.sourceforge.net>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License version 2 as published by the Free Software Foundation.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public License
   along with this library; see the file COPYING.LIB.  If not, write to
   the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.
*/

#include "katelinerange.h"

#include <kdebug.h>

LineRange::LineRange()
  : line(-1)
  , virtualLine(-1)
  , startCol(-1)
  , endCol(-1)
  , startX(-1)
  , endX(-1)
  , shiftX(0)
  , dirty(false)
  , viewLine(-1)
  , wrap(false)
  , startsInvisibleBlock(false)
{
}

void LineRange::clear()
{
  line = -1;
  virtualLine = -1;
  startCol = -1;
  endCol = -1;
  startX = -1;
  shiftX = 0;
  endX = -1;
  viewLine = -1;
  wrap = false;
  startsInvisibleBlock = false;
}

int LineRange::getXOffset() const
{
  return startX ? shiftX : 0;
}

void LineRange::debugOutput() const
{
  kdDebug() << "LineRange: line " << line << " cols [" << startCol << " -> " << endCol << "] x [" << startX << " -> " << endX << " off " << shiftX << "] wrap " << wrap << endl;
}
