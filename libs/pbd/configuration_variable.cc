/*
    Copyright (C) 1999-2009 Paul Davis

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/

#include <iostream>

#include "pbd/compose.h"
#include "pbd/configuration_variable.h"
#include "pbd/debug.h"

using namespace std;
using namespace PBD;

void
ConfigVariableBase::add_to_node (XMLNode& node)
{
	const std::string v = get_as_string ();
	DEBUG_TRACE (DEBUG::Configuration, string_compose ("Config variable %1 stored as [%2]\n", _name, v));
	XMLNode* child = new XMLNode ("Option");
	child->add_property ("name", _name);
	child->add_property ("value", v);
	node.add_child_nocopy (*child);
}

bool
ConfigVariableBase::set_from_node (XMLNode const & node)
{
	if (node.name() == "Config" || node.name() == "Canvas" || node.name() == "UI") {

		/* ardour.rc */

		const XMLProperty* prop;
		XMLNodeList nlist;
		XMLNodeConstIterator niter;
		XMLNode* child;

		nlist = node.children();

		for (niter = nlist.begin(); niter != nlist.end(); ++niter) {

			child = *niter;

			if (child->name() == "Option") {
				if ((prop = child->property ("name")) != 0) {
					if (prop->value() == _name) {
						if ((prop = child->property ("value")) != 0) {
							set_from_string (prop->value());
							return true;
						}
					}
				}
			}
		}

	} else if (node.name() == "Options") {

		/* session file */

		XMLNodeList olist;
		XMLNodeConstIterator oiter;
		XMLNode* option;
		const XMLProperty* opt_prop;

		olist = node.children();

		for (oiter = olist.begin(); oiter != olist.end(); ++oiter) {

			option = *oiter;

			if (option->name() == _name) {
				if ((opt_prop = option->property ("val")) != 0) {
					set_from_string (opt_prop->value());
					return true;
				}
			}
		}
	}

	return false;
}

void
ConfigVariableBase::notify ()
{
	// placeholder for any debugging desired when a config variable is modified
}

void
ConfigVariableBase::miss ()
{
	// placeholder for any debugging desired when a config variable
	// is set but to the same value as it already has
}

