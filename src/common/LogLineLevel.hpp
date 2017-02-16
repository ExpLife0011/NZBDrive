/*
    Copyright (c) 2017, Ole Stauning
    All rights reserved.

    Use of this source code is governed by a GPLv3-style license that can be found
    in the LICENSE file.
*/

#ifndef LOGLINELEVEL_HPP
#define LOGLINELEVEL_HPP

namespace ByteFountain
{
	enum class LogLineLevel
	{ 
		Debug, 
		Info, 
		Warning, 
		Error, 
		Fatal, 
		PopupInfo, 
		PopupError 
	};
}

#endif