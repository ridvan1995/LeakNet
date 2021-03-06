//======== (C) Copyright 1999, 2000 Valve, L.L.C. All rights reserved. ========
//
// The copyright to the contents herein is the property of Valve, L.L.C.
// The contents may be used and/or copied only with the written permission of
// Valve, L.L.C., or in accordance with the terms and conditions stipulated in
// the agreement/contract under which the contents have been supplied.
//
// Purpose: 
//
// $Workfile:     $
// $NoKeywords: $
//=============================================================================

#ifndef MODELINFO_H
#define MODELINFO_H

#ifdef _WIN32
#pragma once
#endif

#include "engine/ivmodelinfo.h"


extern IVModelInfo *modelinfo;			// server version
extern IVModelInfo *modelinfoclient;	// client version


#endif // MODELINFO_H
