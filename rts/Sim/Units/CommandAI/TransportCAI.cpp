#include "StdAfx.h"
#include "TransportCAI.h"
#include "LineDrawer.h"
#include "Sim/Units/UnitHandler.h"
#include "Sim/Units/Unit.h"
#include "Sim/Units/COB/CobInstance.h"
#include "Sim/Units/UnitDef.h"
#include "Sim/Units/UnitTypes/TransportUnit.h"
#include "Map/Ground.h"
#include "Sim/Misc/QuadField.h"
#include "Game/UI/CommandColors.h"
#include "Game/UI/CursorIcons.h"
#include "LogOutput.h"
#include "Rendering/GL/myGL.h"
#include "Rendering/GL/glExtra.h"
#include "Game/GameHelper.h"
#include "Sim/MoveTypes/TAAirMoveType.h"
#include "Sim/ModInfo.h"
#include "Rendering/UnitModels/3DOParser.h"
#include "mmgr.h"
#include "creg/STL_List.h"

static void ScriptCallback(int retCode,void* p1,void* p2)
{
	((CTransportCAI*)p1)->ScriptReady();
}

CR_BIND_DERIVED(CTransportCAI,CMobileCAI , );

CR_REG_METADATA(CTransportCAI, (
				CR_MEMBER(toBeTransportedUnitId),
				CR_MEMBER(scriptReady),
				CR_MEMBER(lastCall),
				CR_MEMBER(unloadType),
				CR_MEMBER(dropSpots),
				CR_MEMBER(isFirstIteration),
				CR_MEMBER(lastDropPos),
				CR_MEMBER(approachVector),
				CR_MEMBER(endDropPos),
				CR_RESERVED(16)
				));

CTransportCAI::CTransportCAI()
: CMobileCAI(),
	lastCall(0),
	scriptReady(false),
	toBeTransportedUnitId(-1)
{}


CTransportCAI::CTransportCAI(CUnit* owner)
: CMobileCAI(owner),
	lastCall(0),
	scriptReady(false),
	toBeTransportedUnitId(-1)
{
	//for new transport methods
	dropSpots.clear();
	approachVector= float3(0,0,0);
	unloadType = owner->unitDef->transportUnloadMethod;
	startingDropPos = float3(-1,-1,-1);
	lastDropPos = float3(-1,-1,-1);
	endDropPos = float3(-1,-1,-1);
	isFirstIteration = true;
	//
	CommandDescription c;
	c.id=CMD_LOAD_UNITS;
	c.action="loadunits";
	c.type=CMDTYPE_ICON_UNIT_OR_AREA;
	c.name="Load units";
	c.mouseicon=c.name;
	c.hotkey="l";
	c.tooltip="Sets the transport to load a unit or units within an area";
	possibleCommands.push_back(c);

	c.id=CMD_UNLOAD_UNITS;
	c.action="unloadunits";
	c.type=CMDTYPE_ICON_AREA;
	c.name="Unload units";
	c.mouseicon=c.name;
	c.hotkey="u";
	c.tooltip="Sets the transport to unload units in an area";
	possibleCommands.push_back(c);
}

CTransportCAI::~CTransportCAI(void)
{
	if(toBeTransportedUnitId!=-1){
		if(uh->units[toBeTransportedUnitId])
			uh->units[toBeTransportedUnitId]->toBeTransported=false;
		toBeTransportedUnitId=-1;
	}
}

void CTransportCAI::SlowUpdate(void)
{
	if(commandQue.empty()){
		CMobileCAI::SlowUpdate();
		return;
	}
	Command& c=commandQue.front();
	switch(c.id){
		case CMD_LOAD_UNITS:   { ExecuteLoadUnits(c); dropSpots.clear(); return; }
		case CMD_UNLOAD_UNITS: { ExecuteUnloadUnits(c); return; }
		case CMD_UNLOAD_UNIT:  { ExecuteUnloadUnit(c);  return; }
		default:{
			dropSpots.clear();
			CMobileCAI::SlowUpdate();
			return;
		}
	}
}

void CTransportCAI::ExecuteLoadUnits(Command &c)
{
	CTransportUnit* transport=(CTransportUnit*)owner;
	if(c.params.size()==1){		//load single unit
		if(transport->transportCapacityUsed >= owner->unitDef->transportCapacity){
			FinishCommand();
			return;
		}
		CUnit* unit=uh->units[(int)c.params[0]];
		if (!unit) {
			FinishCommand();
			return;
		}
		if(c.options & INTERNAL_ORDER) {
			if(unit->commandAI->commandQue.empty()){
				if(!LoadStillValid(unit)){
					FinishCommand();
					return;
				}
			} else {
				Command & currentUnitCommand = unit->commandAI->commandQue[0];
				if(currentUnitCommand.id == CMD_LOAD_ONTO && currentUnitCommand.params.size() == 1 && int(currentUnitCommand.params[0]) == owner->id){
					if((unit->moveType->progressState == CMoveType::Failed) && (owner->moveType->progressState == CMoveType::Failed)){
						unit->commandAI->FinishCommand();
						FinishCommand();
						return;
					}
				} else if(!LoadStillValid(unit)) {
					FinishCommand();
					return;
				}
			}
		}
		if(inCommand){
			if(!owner->cob->busy)
				FinishCommand();
			return;
		}
		if(unit && CanTransport(unit) && UpdateTargetLostTimer(int(c.params[0]))){
			toBeTransportedUnitId=unit->id;
			unit->toBeTransported=true;
			if(unit->mass+transport->transportMassUsed > owner->unitDef->transportMass){
				FinishCommand();
				return;
			}
			if(goalPos.distance2D(unit->pos)>10){
				float3 fix = unit->pos;
				SetGoal(fix,owner->pos,64);
			}
			if(unit->pos.distance2D(owner->pos)<owner->unitDef->loadingRadius*0.9f){
				if(CTAAirMoveType* am=dynamic_cast<CTAAirMoveType*>(owner->moveType)){		//handle air transports differently
					float3 wantedPos=unit->pos+UpVector*unit->model->height;
					SetGoal(wantedPos,owner->pos);
					am->dontCheckCol=true;
					am->ForceHeading(unit->heading);
					am->SetWantedAltitude(unit->model->height);
					am->maxDrift=1;
					//logOutput.Print("cai dist %f %f %f",owner->pos.distance(wantedPos),owner->pos.distance2D(wantedPos),owner->pos.y-wantedPos.y);
					if(owner->pos.distance(wantedPos)<4 && abs(owner->heading-unit->heading)<50 && owner->updir.dot(UpVector)>0.995f){
						am->dontCheckCol=false;
						am->dontLand=true;
						std::vector<int> args;
						args.push_back((int)(unit->model->height*65536));
						owner->cob->Call("BeginTransport",args);
						std::vector<int> args2;
						args2.push_back(0);
						args2.push_back((int)(unit->model->height*65536));
						owner->cob->Call("QueryTransport",args2);
						((CTransportUnit*)owner)->AttachUnit(unit,args2[0]);
						am->SetWantedAltitude(0);
						FinishCommand();
						return;
					}
				} else {
					inCommand=true;
					scriptReady=false;
					StopMove();
					std::vector<int> args;
					args.push_back(unit->id);
					owner->cob->Call("TransportPickup",args,ScriptCallback,this,0);
				}
			}
		} else {
			FinishCommand();
		}
	} else if(c.params.size()==4){		//load in radius
		if(lastCall==gs->frameNum)	//avoid infinite loops
			return;
		lastCall=gs->frameNum;
		float3 pos(c.params[0],c.params[1],c.params[2]);
		float radius=c.params[3];
		CUnit* unit=FindUnitToTransport(pos,radius);
		if(unit && ((CTransportUnit*)owner)->transportCapacityUsed < owner->unitDef->transportCapacity){
			Command c2;
			c2.id=CMD_LOAD_UNITS;
			c2.params.push_back(unit->id);
			c2.options=c.options | INTERNAL_ORDER;
			commandQue.push_front(c2);
			inCommand=false;
			SlowUpdate();
			return;
		} else {
			FinishCommand();
			return;
		}
	}
	isFirstIteration=true;
	startingDropPos = float3(-1,-1,-1); 

	return;
}

void CTransportCAI::ExecuteUnloadUnits(Command &c)
{
	//new Methods
	CTransportUnit* transport=(CTransportUnit*)owner;

	switch(unloadType) {
			case UNLOAD_LAND: UnloadUnits_Land(c,transport); break;

			case UNLOAD_DROP: 
							if (owner->unitDef->canfly)
								UnloadUnits_Drop(c,transport);
							else 
								UnloadUnits_Land(c,transport);
							break;

			case UNLOAD_LANDFLOOD: UnloadUnits_LandFlood(c,transport); break;			
			
			default:UnloadUnits_Land(c,transport); break;
	}		
}

void CTransportCAI::ExecuteUnloadUnit(Command &c)
{
	CTransportUnit* transport = (CTransportUnit*)owner;
	//new methods
	switch (unloadType) {
		case UNLOAD_LAND: UnloadLand(c); break;

		case UNLOAD_DROP: 
						if (owner->unitDef->canfly)
							UnloadDrop(c);
						else 
							UnloadLand(c);
						break;
			
		case UNLOAD_LANDFLOOD: UnloadLandFlood(c); break;

		default: UnloadLand(c); break;
	}
}

void CTransportCAI::ScriptReady(void)
{
	scriptReady = true; // NOTE: does not seem to be used
}

bool CTransportCAI::CanTransport(CUnit* unit)
{
	CTransportUnit* transport=(CTransportUnit*)owner;

	if(unit->mass>=100000 || unit->beingBuilt)
		return false;
	// don't transport cloaked enemies
	if (unit->isCloaked && !gs->AlliedTeams(unit->team, owner->team))
		return false;
	if(unit->unitDef->canhover && (modInfo->transportHover==0))
 		return false;
	if(unit->unitDef->floater && (modInfo->transportShip==0))
		return false;
	if(unit->unitDef->canfly && (modInfo->transportAir==0))
		return false;
	// if not a hover, not a floater and not a flier, then it's probably ground unit
	if(!unit->unitDef->canhover && !unit->unitDef->floater && !unit->unitDef->canfly && (modInfo->transportGround==0))
		return false;
	if(unit->xsize > owner->unitDef->transportSize*2)
		return false;
	if(!transport->CanTransport(unit))
		return false;
	if(unit->mass+transport->transportMassUsed > owner->unitDef->transportMass)
		return false;

	return true;
}

bool CTransportCAI::FindEmptySpot(float3 center, float radius,float emptyRadius, float3& found, CUnit* unitToUnload)
{
//	std::vector<CUnit*> units=qf->GetUnitsExact(center,radius);
	if (dynamic_cast<CTAAirMoveType*>(owner->moveType)) { //handle air transports differently
		for (int a=0;a<100;++a) {
			float3 delta(1,0,1);
			while(delta.SqLength2D()>1){
				delta.x=(gs->randFloat()-0.5f)*2;
				delta.z=(gs->randFloat()-0.5f)*2;
			}
			float3 pos=center+delta*radius;
			pos.y=ground->GetHeight(pos.x,pos.z);

			float unloadPosHeight=ground->GetApproximateHeight(pos.x,pos.z);
			if(unloadPosHeight<(0-unitToUnload->unitDef->maxWaterDepth))
 				continue;
			if(unloadPosHeight>(0-unitToUnload->unitDef->minWaterDepth))
				continue;
			//Don't unload anything on slopes
			if(unitToUnload->unitDef->movedata
					&& ground->GetSlope(pos.x,pos.z) > unitToUnload->unitDef->movedata->maxSlope)
				continue;
			if(!qf->GetUnitsExact(pos,emptyRadius+8).empty())
				continue;
			found=pos;
			return true;
		}
	} else {
		for(float y=max(0.0f,center.z-radius);y<min(float(gs->mapx*SQUARE_SIZE),center.z+radius);y+=SQUARE_SIZE){
			float dy=y-center.z;
			float rx=radius*radius-dy*dy;
			if(rx<=0)
				continue;
			rx=sqrt(rx);
			for(float x=max(0.0f,center.x-rx);x<min(float(gs->mapx*SQUARE_SIZE),center.x+rx);x+=SQUARE_SIZE){
				float unloadPosHeight=ground->GetApproximateHeight(x,y);
				if(unloadPosHeight<(0-unitToUnload->unitDef->maxWaterDepth))
 					continue;
				if(unloadPosHeight>(0-unitToUnload->unitDef->minWaterDepth))
					continue;
				//Don't unload anything on slopes
				if(unitToUnload->unitDef->movedata
						&& ground->GetSlope(x,y) > unitToUnload->unitDef->movedata->maxSlope)
					continue;
				float3 pos(x,ground->GetApproximateHeight(x,y),y);
				if(!qf->GetUnitsExact(pos,emptyRadius+8).empty())
					continue;
				found=pos;
				return true;
			}
		}
	}
	return false;
}

bool CTransportCAI::SpotIsClear(float3 pos,CUnit* unitToUnload) {
	float unloadPosHeight=ground->GetApproximateHeight(pos.x,pos.z);
	
	if(unloadPosHeight<(0-unitToUnload->unitDef->maxWaterDepth))
 		return false;
	if(unloadPosHeight>(0-unitToUnload->unitDef->minWaterDepth))
		return false;
	if(ground->GetSlope(pos.x,pos.z) > unitToUnload->unitDef->movedata->maxSlope)
		return false;
	if(!qf->GetUnitsExact(pos,unitToUnload->radius+8).empty())
		return false;
 
	return true;
}
bool CTransportCAI::FindEmptyDropSpots(float3 startpos, float3 endpos, std::list<float3>& dropSpots) {
	//should only be used by air
	
	CTransportUnit* transport=(CTransportUnit*)owner;
	//dropSpots.clear();
	float gap = 25.5; //TODO - set tag for this?
	float3 dir = endpos - startpos; 
	dir.Normalize();
		
	float3 nextPos = startpos;
	float3 pos;	
	
	list<CTransportUnit::TransportedUnit>::iterator ti = transport->transported.begin();		
	dropSpots.push_front(nextPos);
		
	//first spot
	if (ti!=transport->transported.end()) {		
		//float3 p = nextPos; //test to make intended land spots visible
		//inMapDrawer->CreatePoint(p,ti->unit->unitDef->name);
		//p.z +=transport->transportCapacityUsed*5; 
		nextPos += dir*(gap + ti->unit->radius);
		ti++;					
	}
	
	//remaining spots
	if(CTAAirMoveType* am=dynamic_cast<CTAAirMoveType*>(owner->moveType)){
		while (ti!=transport->transported.end() && startpos.distance(nextPos) < startpos.distance(endpos)) {
			nextPos += dir*(ti->unit->radius);			
			nextPos.y=ground->GetHeight(nextPos.x,nextPos.z);

			//check landing spot is ok for landing on
			if(!SpotIsClear(nextPos,ti->unit)) 
				continue;
						
			dropSpots.push_front(nextPos);								
			//float3 p = nextPos; //test to make intended land spots visible
			//inMapDrawer->CreatePoint(p,ti->unit->unitDef->name);
			//p.z +=transport->transportCapacityUsed*5; 
			nextPos += dir*(gap + ti->unit->radius);
			ti++;		
		}		
		return true;
	}		
		return false;
}

bool CTransportCAI::FindEmptyFloodSpots(float3 startpos, float3 endpos, std::list<float3>& dropSpots, std::vector<float3> exitDirs) {
//select suitable spots according to directions we are allowed to exit transport from
	//TODO
	return false;
}
//
//
void CTransportCAI::UnloadUnits_Land(Command& c, CTransportUnit* transport) {
	if(lastCall==gs->frameNum)	//avoid infinite loops
		return;
	lastCall=gs->frameNum;
	if(((CTransportUnit*)owner)->transported.empty()){
		FinishCommand();
		return;
	}
	float3 pos(c.params[0],c.params[1],c.params[2]);
	float radius=c.params[3];
	float3 found;
	//((CTransportUnit*)owner)->transported

	bool canUnload=FindEmptySpot(pos,max(16.0f,radius),((CTransportUnit*)owner)->transported.front().unit->radius,found,((CTransportUnit*)owner)->transported.front().unit);
	if(canUnload){
		Command c2;
		c2.id=CMD_UNLOAD_UNIT;
		c2.params.push_back(found.x);
		c2.params.push_back(found.y);
		c2.params.push_back(found.z);
		c2.options=c.options | INTERNAL_ORDER;
		commandQue.push_front(c2);
		SlowUpdate();
		return;
	} else {
		FinishCommand();
	}
	return;
	
}
void CTransportCAI::UnloadUnits_Drop(Command& c, CTransportUnit* transport) {
	//called repeatedly for each unit till units are unloaded		
		if(lastCall==gs->frameNum)	//avoid infinite loops
			return;
		lastCall=gs->frameNum;

		if(((CTransportUnit*)owner)->transported.empty() ){
			FinishCommand();
			return;
		}
				
		float3 pos(c.params[0],c.params[1],c.params[2]);
		float radius=c.params[3];		
		bool canUnload = false;
					
		//at the start of each user command
		if (isFirstIteration )	{ 					
			dropSpots.clear();
			startingDropPos = pos;
									
			approachVector = startingDropPos-owner->pos;
			approachVector.Normalize();
			canUnload = FindEmptyDropSpots(pos, pos + approachVector*max(16.0f,radius), dropSpots);

		} else if (!dropSpots.empty() ) {
			//make sure we check current spot infront of us each unload
			pos = dropSpots.back(); //take last landing pos as new start spot
			canUnload = dropSpots.size() > 0;			
		}

		if( canUnload ){
			if(SpotIsClear(dropSpots.back(),((CTransportUnit*)owner)->transported.front().unit)) {
				float3 pos = dropSpots.back();
				Command c2;
				c2.id=CMD_UNLOAD_UNIT;
				c2.params.push_back(pos.x);
				c2.params.push_back(pos.y);
				c2.params.push_back(pos.z);				
				c2.options=c.options | INTERNAL_ORDER;
				commandQue.push_front(c2);
				
				SlowUpdate();
				isFirstIteration = false;	
				return;
			} else {
				dropSpots.pop_back();
			}
		} else {		
			
			startingDropPos = float3(-1,-1,-1);	
			isFirstIteration=true;
			dropSpots.clear();
			FinishCommand();
		}
}
void CTransportCAI::UnloadUnits_CrashFlood(Command& c, CTransportUnit* transport) {
	//TODO - fly into the ground, doing damage to units at landing pos, then unload.
	//needs heavy modification of TAAirMoveType
}
void CTransportCAI::UnloadUnits_LandFlood(Command& c, CTransportUnit* transport) {
	if(lastCall==gs->frameNum)	//avoid infinite loops
		return;
	lastCall=gs->frameNum;
	if(((CTransportUnit*)owner)->transported.empty()){
	FinishCommand();
		return;
	}
	float3 pos(c.params[0],c.params[1],c.params[2]);
	float radius=c.params[3];
	float3 found;
	//((CTransportUnit*)owner)->transported
	
	bool canUnload=FindEmptySpot(pos,max(16.0f,radius),((CTransportUnit*)owner)->transported.front().unit->radius,found,((CTransportUnit*)owner)->transported.front().unit);
	if(canUnload){
		
		Command c2;
		c2.id=CMD_UNLOAD_UNIT;
		c2.params.push_back(found.x);
		c2.params.push_back(found.y);
		c2.params.push_back(found.z);
		c2.options=c.options | INTERNAL_ORDER;
		commandQue.push_front(c2);

		if (isFirstIteration )	{
			Command c1;
			c1.id=CMD_MOVE;
			c1.params.push_back(pos.x);
			c1.params.push_back(pos.y);
			c1.params.push_back(pos.z);
			c1.options=c.options | INTERNAL_ORDER;
			commandQue.push_front(c1);
			startingDropPos = pos;			
		}		

		SlowUpdate();
		return;
	} else {
		FinishCommand();
	}
	return;
	
}


//
void CTransportCAI::UnloadLand(Command& c) {	
	//default unload
	CTransportUnit* transport = (CTransportUnit*)owner;
	if (inCommand) {
			if (!owner->cob->busy) {
		//			if(scriptReady)
				FinishCommand();
			}
	} else {
		const std::list<CTransportUnit::TransportedUnit>& transList =
		  transport->transported;

		if (transList.empty()) {
			FinishCommand();
			return;
		}

		float3 pos(c.params[0], c.params[1], c.params[2]);
		if(goalPos.distance2D(pos) > 20){
			SetGoal(pos, owner->pos);
		}

		CUnit* unit = NULL;
		if (c.params.size() < 4) {
			unit = transList.front().unit;
		} else {
			const int unitID = (int)c.params[3];
			std::list<CTransportUnit::TransportedUnit>::const_iterator it;
			for (it = transList.begin(); it != transList.end(); ++it) {
				CUnit* carried = it->unit;
				if (unitID == carried->id) {
					unit = carried;
					break;
				}
			}
			if (unit == NULL) {
				FinishCommand();
				return;
			}
		}

		if (pos.distance2D(owner->pos) < (owner->unitDef->loadingRadius * 0.9f)) {
			CTAAirMoveType* am = dynamic_cast<CTAAirMoveType*>(owner->moveType);
			if (am != NULL) {
				// handle air transports differently
				pos.y = ground->GetHeight(pos.x, pos.z);
				const float3 wantedPos = pos + UpVector * unit->model->height;
				SetGoal(wantedPos, owner->pos);
				am->SetWantedAltitude(unit->model->height);
				am->maxDrift = 1;
				if ((owner->pos.distance(wantedPos) < 8) &&
					(owner->updir.dot(UpVector) > 0.99f)) {
					transport->DetachUnit(unit);
					if (transport->transported.empty()) {
						am->dontLand = false;
						owner->cob->Call("EndTransport");
					}
					const float3 fix = owner->pos + owner->frontdir * 20;
					SetGoal(fix, owner->pos);		//move the transport away slightly
					FinishCommand();
				}
			} else {
				inCommand = true;
				scriptReady = false;
				StopMove();
				std::vector<int> args;
				args.push_back(transList.front().unit->id);
				args.push_back(PACKXZ(pos.x, pos.z));
				owner->cob->Call("TransportDrop", args, ScriptCallback, this, 0);
			}
		}
	}
	return;	

}
void CTransportCAI::UnloadDrop(Command& c) {
	
	//fly over and drop unit
	if(inCommand){
		if(!owner->cob->busy)
			//if(scriptReady)
			FinishCommand();
	} else {
		if(((CTransportUnit*)owner)->transported.empty()){
			FinishCommand();
			return;
		}

		float3 pos(c.params[0],c.params[1],c.params[2]); //head towards goal
		
		//note that taairmovetype must be modified to allow non stop movement through goals for this to work well
		if(goalPos.distance2D(pos)>20){
			SetGoal(pos,owner->pos);
			lastDropPos = pos;
		}
		
		if(CTAAirMoveType* am=dynamic_cast<CTAAirMoveType*>(owner->moveType)){
	
			pos.y=ground->GetHeight(pos.x,pos.z);
			CUnit* unit=((CTransportUnit*)owner)->transported.front().unit;				
			am->maxDrift=1;

			//if near target or have past it accidentally- drop unit					
			if(owner->pos.distance2D(pos) < 40 || (((pos - owner->pos).Normalize()).distance(owner->frontdir.Normalize()) > 0.5 && owner->pos.distance2D(pos)< 205)) { 									
				am->dontLand=true;
				owner->cob->Call("EndTransport"); //test
				((CTransportUnit*)owner)->DetachUnitFromAir(unit,pos);		
				dropSpots.pop_back();

				if (dropSpots.empty()) { 
					float3 fix = owner->pos+owner->frontdir*200;
					SetGoal(fix,owner->pos);//move the transport away after last drop
				}
				FinishCommand();
			}			
		} else {
			inCommand=true;
			scriptReady=false;
			StopMove();
			std::vector<int> args;
			args.push_back(((CTransportUnit*)owner)->transported.front().unit->id);
			args.push_back(PACKXZ(pos.x, pos.z));
			owner->cob->Call("TransportDrop",args,ScriptCallback,this,0);
		}		
	}	
}
void CTransportCAI::UnloadCrashFlood(Command& c) {
	//TODO - will require heavy modification of TAAirMoveType.cpp
}
void CTransportCAI::UnloadLandFlood(Command& c) {
	//land, then release all units at once
	CTransportUnit* transport = (CTransportUnit*)owner;
	if (inCommand) {
			if (!owner->cob->busy) {
			  //if(scriptReady)
				FinishCommand();
			}
	} else {
		const std::list<CTransportUnit::TransportedUnit>& transList =
		  transport->transported;

		if (transList.empty()) {
			FinishCommand();
			return;
		}
		
		//check units are all carried
		CUnit* unit = NULL;
		if (c.params.size() < 4) {
			unit = transList.front().unit;
		} else {
			const int unitID = (int)c.params[3];
			std::list<CTransportUnit::TransportedUnit>::const_iterator it;
			for (it = transList.begin(); it != transList.end(); ++it) {
				CUnit* carried = it->unit;
				if (unitID == carried->id) {
					unit = carried;
					break;
				}
			}
			if (unit == NULL) {
				FinishCommand();
				return;
			}
		}
		
		//move to position
		float3 pos(c.params[0], c.params[1], c.params[2]);
		if (isFirstIteration) {			
			if(goalPos.distance2D(pos) > 20)								
				SetGoal(startingDropPos, owner->pos);			
		}

		if (startingDropPos.distance2D(owner->pos) < (owner->unitDef->loadingRadius * 0.9f)) {
			//create aircraft movetype instance
			CTAAirMoveType* am = dynamic_cast<CTAAirMoveType*>(owner->moveType);

			if (am != NULL) {				
				//lower to ground	
			
				startingDropPos.y = ground->GetHeight(startingDropPos.x,startingDropPos.z);				
				const float3 wantedPos = startingDropPos + UpVector * unit->model->height;
				SetGoal(wantedPos, owner->pos);
				am->SetWantedAltitude(1);
				am->maxDrift = 1;
				am->dontLand = false;	

				//when on our way down start animations for unloading gear
				if (isFirstIteration) {
					owner->cob->Call("StartUnload");					
				}
				isFirstIteration = false;

				//once at ground
				if (owner->pos.y - ground->GetHeight(wantedPos.x,wantedPos.z) < 8) {
															
					am->SetState(am->AIRCRAFT_LANDED);//nail it to the ground before it tries jumping up, only to land again...
					std::vector<int> args;
					args.push_back(transList.front().unit->id);
					args.push_back(PACKXZ(pos.x, pos.z));
					owner->cob->Call("TransportDrop", args, ScriptCallback, this, 0); //call this so that other animations such as opening doors may be started
					transport->DetachUnitFromAir(unit,pos);				
										
					FinishCommand();									
					if (transport->transported.empty()) {
						am->dontLand = false;
						owner->cob->Call("EndTransport");
						am->UpdateLanded();
					}
				}				
			} else {
				
				//land transports
				inCommand = true;
				scriptReady = false;
				StopMove();
				std::vector<int> args;
				args.push_back(transList.front().unit->id);
				args.push_back(PACKXZ(pos.x, pos.z));
				owner->cob->Call("TransportDrop", args, ScriptCallback, this, 0);
				transport->DetachUnitFromAir(unit,pos);	
				isFirstIteration = false;
				FinishCommand();
				if (transport->transported.empty()) 						
					owner->cob->Call("EndTransport");											
			}
		}
	}
	return;	
}
CUnit* CTransportCAI::FindUnitToTransport(float3 center, float radius)
{
	CUnit* best=0;
	float bestDist=100000000;
	std::vector<CUnit*> units=qf->GetUnitsExact(center,radius);
	for(std::vector<CUnit*>::iterator ui=units.begin();ui!=units.end();++ui){
		CUnit* unit=(*ui);
		float dist=unit->pos.distance2D(owner->pos);
		if(CanTransport(unit) && dist<bestDist && !unit->toBeTransported &&
				 (unit->losStatus[owner->allyteam] & (LOS_INRADAR|LOS_INLOS))){
			bestDist=dist;
			best=unit;
		}
	}
	return best;
}

int CTransportCAI::GetDefaultCmd(CUnit* pointed, CFeature* feature)
{
	if (pointed) {
		if (!gs->Ally(gu->myAllyTeam, pointed->allyteam)) {
			if (owner->unitDef->canAttack) {
				return CMD_ATTACK;
			} else if (CanTransport(pointed)) {
				return CMD_LOAD_UNITS; // comm napping?
			}
		} else {
			if (CanTransport(pointed)) {
				return CMD_LOAD_UNITS;
			} else if (owner->unitDef->canGuard) {
				return CMD_GUARD;
			}
		}
	}
//	if(((CTransportUnit*)owner)->transported.empty())
	if (owner->unitDef->canmove) {
		return CMD_MOVE;
	} else {
		return CMD_STOP;
	}
//	else
//		return CMD_UNLOAD_UNITS;
}

void CTransportCAI::DrawCommands(void)
{
	lineDrawer.StartPath(owner->midPos, cmdColors.start);

	if (owner->selfDCountdown != 0) {
		lineDrawer.DrawIconAtLastPos(CMD_SELFD);
	}

	CCommandQueue::iterator ci;
	for(ci=commandQue.begin();ci!=commandQue.end();++ci){
		switch(ci->id){
			case CMD_MOVE:{
				const float3 endPos(ci->params[0],ci->params[1],ci->params[2]);
				lineDrawer.DrawLineAndIcon(ci->id, endPos, cmdColors.move);
				break;
			}
			case CMD_FIGHT:{
				if (ci->params.size() >= 3) {
					const float3 endPos(ci->params[0],ci->params[1],ci->params[2]);
					lineDrawer.DrawLineAndIcon(ci->id, endPos, cmdColors.fight);
				}
				break;
			}
			case CMD_PATROL:{
				const float3 endPos(ci->params[0],ci->params[1],ci->params[2]);
				lineDrawer.DrawLineAndIcon(ci->id, endPos, cmdColors.patrol);
				break;
			}
			case CMD_ATTACK:{
				if(ci->params.size()==1){
					const CUnit* unit = uh->units[int(ci->params[0])];
					if((unit != NULL) && isTrackable(unit)) {
						const float3 endPos =
							helper->GetUnitErrorPos(unit, owner->allyteam);
						lineDrawer.DrawLineAndIcon(ci->id, endPos, cmdColors.attack);
					}
				} else {
					const float3 endPos(ci->params[0],ci->params[1],ci->params[2]);
					lineDrawer.DrawLineAndIcon(ci->id, endPos, cmdColors.attack);
				}
				break;
			}
			case CMD_GUARD:{
				const CUnit* unit = uh->units[int(ci->params[0])];
				if((unit != NULL) && isTrackable(unit)) {
					const float3 endPos =
						helper->GetUnitErrorPos(unit, owner->allyteam);
					lineDrawer.DrawLineAndIcon(ci->id, endPos, cmdColors.guard);
				}
				break;
			}
			case CMD_LOAD_UNITS:{
				if(ci->params.size()==4){
					const float3 endPos(ci->params[0],ci->params[1],ci->params[2]);
					lineDrawer.DrawLineAndIcon(ci->id, endPos, cmdColors.load);
					lineDrawer.Break(endPos, cmdColors.load);
					glSurfaceCircle(endPos, ci->params[3], 20);
					lineDrawer.RestartSameColor();
				} else {
					const CUnit* unit = uh->units[int(ci->params[0])];
					if((unit != NULL) && isTrackable(unit)) {
						const float3 endPos =
							helper->GetUnitErrorPos(unit, owner->allyteam);
						lineDrawer.DrawLineAndIcon(ci->id, endPos, cmdColors.load);
					}
				}
				break;
			}
			case CMD_UNLOAD_UNITS:{
				if(ci->params.size()==4){
					const float3 endPos(ci->params[0],ci->params[1],ci->params[2]);
					lineDrawer.DrawLineAndIcon(ci->id, endPos, cmdColors.unload);
					lineDrawer.Break(endPos, cmdColors.unload);
					glSurfaceCircle(endPos, ci->params[3], 20);
					lineDrawer.RestartSameColor();
				}
				break;
			}
			case CMD_UNLOAD_UNIT:{
				const float3 endPos(ci->params[0],ci->params[1],ci->params[2]);
				lineDrawer.DrawLineAndIcon(ci->id, endPos, cmdColors.unload);
				break;
			}
			case CMD_WAIT:{
				DrawWaitIcon(*ci);
				break;
			}
			case CMD_SELFD:{
				lineDrawer.DrawIconAtLastPos(ci->id);
				break;
			}
		}
	}
	lineDrawer.FinishPath();
}


void CTransportCAI::FinishCommand(void)
{
	if(CTAAirMoveType* am=dynamic_cast<CTAAirMoveType*>(owner->moveType))
		am->dontCheckCol=false;

	if(toBeTransportedUnitId!=-1){
		if(uh->units[toBeTransportedUnitId])
			uh->units[toBeTransportedUnitId]->toBeTransported=false;
		toBeTransportedUnitId=-1;
	}
	CMobileCAI::FinishCommand();
}

bool CTransportCAI::LoadStillValid(CUnit* unit){
	if(commandQue.size() < 2){
		return false;
	}
	Command cmd = commandQue[1];
	return !(cmd.id == CMD_LOAD_UNITS && cmd.params.size() == 4
		&& unit->pos.distance2D(
		float3(cmd.params[0], cmd.params[1], cmd.params[2])) > cmd.params[3]*2);
}
