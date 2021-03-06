/*
libfive: a CAD kernel for modeling with implicit functions
Copyright (C) 2017  Matt Keeter

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/
#include <iostream>
#include <cassert>

#include "libfive/tree/archive.hpp"
#include "libfive/tree/serializer.hpp"
#include "libfive/tree/deserializer.hpp"

namespace Kernel
{
void Archive::addShape(Tree tree, std::string name, std::string doc,
                       std::map<Tree::Id, std::string> vars)
{
    Shape s;
    s.tree = tree;
    s.name = name;
    s.doc = doc;
    s.vars = vars;

    shapes.push_back(s);
}

////////////////////////////////////////////////////////////////////////////////

void Archive::serialize(std::ostream& out)
{
    Serializer(out).run(*this);
}

Archive Archive::deserialize(std::istream& data)
{
    return Deserializer(data).run();
}


}   // namespace Kernel
