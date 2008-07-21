/*
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

#ifndef _AIUNITCAPTUREDEVENT_H
#define	_AIUNITCAPTUREDEVENT_H

#include "ExternalAI/IGlobalAI.h"

class CAIUnitCapturedEvent : public CAIEvent {
public:
    CAIUnitCapturedEvent(SUnitCapturedEvent* event): event(*event) {}
    ~CAIUnitCapturedEvent() {}
    
    void run(CAI* ai) {
		int evtId = AI_EVENT_UNITCAPTURED;
		IGlobalAI::ChangeTeamEvent evt = {event.unitId, event.newTeamId, event.oldTeamId};
        ((CAIGlobalAI*) ai)->gai->HandleEvent(evtId, &evt);
    }
private:
    SUnitCapturedEvent event;
};

#endif	/* _AIUNITCAPTUREDEVENT_H */

