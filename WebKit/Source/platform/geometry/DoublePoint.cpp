// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/geometry/DoublePoint.h"

#include <algorithm>
#include "platform/geometry/FloatSize.h"
#include "platform/geometry/LayoutPoint.h"
#include "platform/wtf/text/WTFString.h"

namespace blink {

DoublePoint::DoublePoint(const LayoutPoint& p)
    : x_(p.X().ToDouble()), y_(p.Y().ToDouble()) {}

DoublePoint::DoublePoint(const FloatSize& size)
    : x_(size.Width()), y_(size.Height()) {}

DoublePoint DoublePoint::ExpandedTo(const DoublePoint& other) const {
  return DoublePoint(std::max(x_, other.x_), std::max(y_, other.y_));
}

DoublePoint DoublePoint::ShrunkTo(const DoublePoint& other) const {
  return DoublePoint(std::min(x_, other.x_), std::min(y_, other.y_));
}

String DoublePoint::ToString() const {
  return String::Format("%lg,%lg", X(), Y());
}

}  // namespace blink
