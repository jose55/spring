/*
    Copyright 2008  Nicolas Wu
    
    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef AILIBRARY_H
#define AILIBRARY_H

#include <string>
#include "Object.h"
#include "Platform/SharedLib.h"

// This class is responsable for loading a particular AI library, and 
// handling handleEvent() messages intended for that member.

class CAILibrary {
public:
    
    CAILibrary(const char* libName,  int team);
    ~CAILibrary();
    
    void init();
    
    int handleEvent(int eventID, void* event);
    
    typedef void (*AI_INIT)(int);
    AI_INIT _init;
    
    typedef int (*AI_HANDLEEVENT)(int, int, void*);
    AI_HANDLEEVENT _handleEvent;
    
    int team;
private: 
    const char* libName;
    SharedLib* lib;
    

};

#endif /*AILIBRARY_H*/
