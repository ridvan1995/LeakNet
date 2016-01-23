//========= Copyright � 1996-2003, Valve LLC, All rights reserved. ============
//
// Purpose: Base class for all animating characters and objects.
//
//=============================================================================

#include "cbase.h"
#include "baseanimating.h"
#include "animation.h"
#include "activitylist.h"
#include "studio.h"
#include "bone_setup.h"
#include "mathlib.h"
#include "model_types.h"
#include "physics.h"
#include "ndebugoverlay.h"
#include "vstdlib/strtools.h"
#include "npcevent.h"
#include "isaverestore.h"

class CIKSaveRestoreOps : public CClassPtrSaveRestoreOps
{
	// save data type interface
	void Save( const SaveRestoreFieldInfo_t &fieldInfo, ISave *pSave )
	{
		Assert( fieldInfo.pTypeDesc->fieldSize == 1 );
		CIKContext **pIK = (CIKContext **)fieldInfo.pField;
		bool bHasIK = (*pIK) != 0;
		pSave->WriteBool( &bHasIK );
	}

	void Restore( const SaveRestoreFieldInfo_t &fieldInfo, IRestore *pRestore )
	{
		Assert( fieldInfo.pTypeDesc->fieldSize == 1 );
		CIKContext **pIK = (CIKContext **)fieldInfo.pField;

		bool bHasIK;
		pRestore->ReadBool( &bHasIK );
		*pIK = (bHasIK) ? new CIKContext : NULL;
	}
};

static CIKSaveRestoreOps s_IKSaveRestoreOp;


BEGIN_DATADESC( CBaseAnimating )

	DEFINE_FIELD( CBaseAnimating, m_flGroundSpeed, FIELD_FLOAT ),
	DEFINE_FIELD( CBaseAnimating, m_flLastEventCheck, FIELD_TIME ),
	DEFINE_FIELD( CBaseAnimating, m_bSequenceFinished, FIELD_BOOLEAN ),
	DEFINE_FIELD( CBaseAnimating, m_bSequenceLoops, FIELD_BOOLEAN ),

//	DEFINE_FIELD( CBaseAnimating, m_nForceBone, FIELD_INTEGER ),
//	DEFINE_FIELD( CBaseAnimating, m_vecForce, FIELD_VECTOR ),

	DEFINE_INPUT( CBaseAnimating, m_nSkin, FIELD_INTEGER, "skin" ),
	DEFINE_KEYFIELD( CBaseAnimating, m_nBody, FIELD_INTEGER, "body" ),
	DEFINE_INPUT( CBaseAnimating, m_nBody, FIELD_INTEGER, "SetBodyGroup" ),
	DEFINE_KEYFIELD( CBaseAnimating, m_nHitboxSet, FIELD_INTEGER, "hitboxset" ),
	DEFINE_KEYFIELD( CBaseAnimating, m_flModelScale, FIELD_FLOAT, "scale" ),
	DEFINE_KEYFIELD( CBaseAnimating, m_nSequence, FIELD_INTEGER, "sequence" ),
	DEFINE_ARRAY( CBaseAnimating,	m_flPoseParameter, FIELD_FLOAT, CBaseAnimating::NUM_POSEPAREMETERS ),
	DEFINE_ARRAY( CBaseAnimating,	m_flEncodedController,	FIELD_FLOAT, CBaseAnimating::NUM_BONECTRLS ),
	DEFINE_KEYFIELD( CBaseAnimating, m_flPlaybackRate, FIELD_FLOAT, "playbackrate" ),
	DEFINE_KEYFIELD( CBaseAnimating, m_flCycle, FIELD_FLOAT, "cycle" ),
	DEFINE_CUSTOM_FIELD( CBaseAnimating, m_pIk, &s_IKSaveRestoreOp ),
	DEFINE_FIELD( CBaseAnimating, m_bClientSideAnimation, FIELD_BOOLEAN ),
	DEFINE_FIELD( CBaseAnimating, m_bClientSideFrameReset, FIELD_BOOLEAN ),
	DEFINE_FIELD( CBaseAnimating, m_nNewSequenceParity, FIELD_INTEGER ),
	DEFINE_FIELD( CBaseAnimating, m_nResetEventsParity, FIELD_INTEGER ),

END_DATADESC()

// Sendtable for fields we don't want to send to clientside animating entities
BEGIN_SEND_TABLE_NOBASE( CBaseAnimating, DT_ServerAnimationData )
	// ANIMATION_CYCLE_BITS is defined in shareddefs.h
	SendPropFloat	(SENDINFO(m_flCycle),		ANIMATION_CYCLE_BITS, SPROP_ROUNDDOWN,	0.0f,   1.0f)
END_SEND_TABLE()

void *SendProxy_ClientSideAnimation( const void *pStruct, const void *pVarData, CSendProxyRecipients *pRecipients, int objectID );

// SendTable stuff.
IMPLEMENT_SERVERCLASS_ST(CBaseAnimating, DT_BaseAnimating)
	SendPropInt		( SENDINFO(m_nForceBone), 8, 0 ),
	SendPropVector	( SENDINFO(m_vecForce), -1, SPROP_NOSCALE ),

	SendPropInt		(SENDINFO(m_nSkin), 10),
	SendPropInt		(SENDINFO(m_nBody), 32),

	SendPropInt		(SENDINFO(m_nHitboxSet),2, SPROP_UNSIGNED ),

	SendPropFloat	(SENDINFO(m_flModelScale),		8,	SPROP_ROUNDUP,	0.0f,	8.0f),
	SendPropArray	(SendPropFloat(SENDINFO_ARRAY(m_flPoseParameter),	11, 0, 0.0f, 1.0f), m_flPoseParameter),
	SendPropInt		(SENDINFO(m_nSequence),	9, 0),
	SendPropFloat	(SENDINFO(m_flPlaybackRate),	8,	SPROP_ROUNDUP,	-4.0,	12.0f), // NOTE: if this isn't a power of 2 than "1.0" can't be encoded correctly
	SendPropArray	(SendPropFloat(SENDINFO_ARRAY(m_flEncodedController), 11, SPROP_ROUNDDOWN, 0.0f, 1.0f ), m_flEncodedController),

	SendPropInt( SENDINFO( m_bClientSideAnimation ), 1, SPROP_UNSIGNED ),
	SendPropInt( SENDINFO( m_bClientSideFrameReset ), 1, SPROP_UNSIGNED ),

	SendPropInt( SENDINFO( m_nNewSequenceParity ), EF_PARITY_BITS, SPROP_UNSIGNED ),
	SendPropInt( SENDINFO( m_nResetEventsParity ), EF_PARITY_BITS, SPROP_UNSIGNED ),

	SendPropDataTable( "serveranimdata", 0, &REFERENCE_SEND_TABLE( DT_ServerAnimationData ), SendProxy_ClientSideAnimation ),
END_SEND_TABLE()


CBaseAnimating::CBaseAnimating()
{
#ifdef _DEBUG
	m_vecForce.GetForModify().Init();
#endif

	m_bClientSideAnimation = false;
	m_pIk = NULL;
}

CBaseAnimating::~CBaseAnimating()
{
	delete m_pIk;
}

void CBaseAnimating::UseClientSideAnimation()
{
	// This should not be called after PostConstructor.
	Assert( !pev );
	m_bClientSideAnimation = true;
}

#define MAX_ANIMTIME_INTERVAL 0.2f

//-----------------------------------------------------------------------------
// Purpose: 
// Output : float
//-----------------------------------------------------------------------------
float CBaseAnimating::GetAnimTimeInterval( void ) const
{
//	float flInterval = clamp( m_flAnimTime - m_flPrevAnimTime, 0, MAX_ANIMTIME_INTERVAL );
	float flInterval;
	if (m_flAnimTime < gpGlobals->curtime)
	{
		// estimate what it'll be this frame
		flInterval = clamp( gpGlobals->curtime - m_flAnimTime, 0, MAX_ANIMTIME_INTERVAL );
	}
	else
	{
		// report actual
		flInterval = clamp( m_flAnimTime - m_flPrevAnimTime, 0, MAX_ANIMTIME_INTERVAL );
	}
	return flInterval;
}

//=========================================================
// StudioFrameAdvance - advance the animation frame up some interval (default 0.1) into the future
//=========================================================
void CBaseAnimating::StudioFrameAdvance()
{
	if ( !m_flPrevAnimTime )
	{
	//	m_flPrevAnimTime = gpGlobals->curtime;
		m_flPrevAnimTime = m_flAnimTime;
	}

	// Time since last animation
	float flInterval = gpGlobals->curtime - m_flAnimTime;
	flInterval = clamp( flInterval, 0, MAX_ANIMTIME_INTERVAL );

	//Msg( "%i %s interval %f\n", entindex(), GetClassname(), flInterval );

	if (flInterval <= 0.001)
	{
		// Msg("%s : %s : %5.3f (skip)\n", GetClassname(), GetSequenceName( m_nSequence ), m_flCycle );
		return;
	}

	// Latch prev
	m_flPrevAnimTime = m_flAnimTime;
	// Set current
	m_flAnimTime = gpGlobals->curtime;
	// Drive cycle
	float flCycleRate = GetSequenceCycleRate( m_nSequence ) * m_flPlaybackRate;

	m_flCycle += flInterval * flCycleRate;

	if (m_flCycle < 0.0 || m_flCycle >= 1.0) 
	{
		if (m_bSequenceLoops)
		{
			m_flCycle -= (int)(m_flCycle);
		}
		else
		{
			m_flCycle = (m_flCycle < 0.0f) ? 0.0f : 0.999f;
		}
		m_bSequenceFinished = true;	// just in case it wasn't caught in GetEvents
	}
	else if (m_flCycle > GetLastVisibleCycle( m_nSequence ))
	{
		m_bSequenceFinished = true;
	}

	/*
	if (!IsPlayer())
		Msg("%s %6.3f : %6.3f %6.3f (%.3f) %.3f\n", 
			GetClassname(), gpGlobals->curtime, 
			m_flAnimTime.Get(), m_flPrevAnimTime, flInterval, m_flCycle.Get() );
	*/

	m_flGroundSpeed = GetSequenceGroundSpeed( m_nSequence );

	// Msg("%s : %s : %5.1f\n", GetClassname(), GetSequenceName( m_nSequence ), m_flCycle );
}

//=========================================================
// SelectWeightedSequence
//=========================================================
int CBaseAnimating::SelectWeightedSequence ( Activity activity )
{
	Assert( activity != ACT_INVALID );
	Assert( GetModelPtr() );
	return ::SelectWeightedSequence( GetModelPtr(), activity, m_nSequence );
}

//=========================================================
// ResetActivityIndexes
//=========================================================
void CBaseAnimating::ResetActivityIndexes ( void )
{
	Assert( GetModelPtr() );
	::ResetActivityIndexes( GetModelPtr() );
}

//=========================================================
// LookupHeaviestSequence
//
// Get sequence with highest 'weight' for this activity
//
//=========================================================
int CBaseAnimating::SelectHeaviestSequence ( Activity activity )
{
	Assert( GetModelPtr() );
	return ::SelectHeaviestSequence( GetModelPtr(), activity );
}


//-----------------------------------------------------------------------------
// Purpose: Looks up an activity by name.
// Input  : label - Name of the activity, ie "ACT_IDLE".
// Output : Returns the activity ID or ACT_INVALID.
//-----------------------------------------------------------------------------
int CBaseAnimating::LookupActivity( const char *label )
{
	Assert( GetModelPtr() );
	return ::LookupActivity( GetModelPtr(), label );
}

//=========================================================
//=========================================================
int CBaseAnimating::LookupSequence( const char *label )
{
	Assert( GetModelPtr() );
	return ::LookupSequence( GetModelPtr(), label );
}

//-----------------------------------------------------------------------------
// Purpose: 
//
// Input  : iSequence - 
//
// Output : float - 
//-----------------------------------------------------------------------------
float CBaseAnimating::GetSequenceMoveYaw( int iSequence )
{
	Vector				vecReturn;
	
	Assert( GetModelPtr() );
	::GetSequenceLinearMotion( GetModelPtr(), iSequence, GetPoseParameterArray(), &vecReturn );

	if (vecReturn.Length() > 0)
	{
		return UTIL_VecToYaw( vecReturn );
	}

	return NOMOTION;
}

//-----------------------------------------------------------------------------
// Purpose: 
//
// Input  : iSequence - 
//
// Output : float
//-----------------------------------------------------------------------------
float CBaseAnimating::GetSequenceMoveDist( int iSequence )
{
	Vector				vecReturn;
	
//	Assert( GetModelPtr() ); // VXP
	::GetSequenceLinearMotion( GetModelPtr(), iSequence, GetPoseParameterArray(), &vecReturn );

	return vecReturn.Length();
}

//-----------------------------------------------------------------------------
// Purpose: 
//
// Input  : iSequence - 
//			*pVec - 
//
//-----------------------------------------------------------------------------
void CBaseAnimating::GetSequenceLinearMotion( int iSequence, Vector *pVec )
{
	Assert( GetModelPtr() );
	::GetSequenceLinearMotion( GetModelPtr(), iSequence, GetPoseParameterArray(), pVec );
}

//-----------------------------------------------------------------------------
// Purpose: 
//
// Input  : iSequence - 
//
// Output : char
//-----------------------------------------------------------------------------
const char *CBaseAnimating::GetSequenceName( int iSequence )
{
	if( iSequence == -1 )
	{
		return "Not Found!";
	}

	if ( !GetModelPtr() )
		return "No model!";

	return ::GetSequenceName( GetModelPtr(), iSequence );
}
//-----------------------------------------------------------------------------
// Purpose: 
//
// Input  : iSequence - 
//
// Output : char
//-----------------------------------------------------------------------------
const char *CBaseAnimating::GetSequenceActivityName( int iSequence )
{
	if( iSequence == -1 )
	{
		return "Not Found!";
	}

	if ( !GetModelPtr() )
		return "No model!";

	return ::GetSequenceActivityName( GetModelPtr(), iSequence );
}


//-----------------------------------------------------------------------------
// Purpose: Make this a client-side simulated entity
// Input  : force - vector of force to be exerted in the physics simulation
//			forceBone - bone to exert force upon
// Output : Returns true on success, false on failure.
//-----------------------------------------------------------------------------
bool CBaseAnimating::BecomeRagdollOnClient( const Vector &force )
{
	// If this character has a ragdoll animation, turn it over to the physics system
	if ( CanBecomeRagdoll() ) 
	{
		VPhysicsDestroyObject();
		AddSolidFlags( FSOLID_NOT_SOLID );
		m_nRenderFX = kRenderFxRagdoll;
		m_vecForce = force;
		SetParent( NULL );

		SetMoveType( MOVETYPE_NONE );
		//UTIL_SetSize( this, vec3_origin, vec3_origin );
		SetThink( NULL );
		Relink();
		return true;
	}
	return false;
}

bool CBaseAnimating::IsRagdoll()
{
	return ( m_nRenderFX == kRenderFxRagdoll ) ? true : false;
}

bool CBaseAnimating::CanBecomeRagdoll( void ) 
{
	int ragdollSequence = SelectWeightedSequence( ACT_DIERAGDOLL );

	//Can't cause we don't have a ragdoll sequence.
	if ( ragdollSequence == ACTIVITY_NOT_AVAILABLE )
		 return false;

	return true;
}

//=========================================================
//=========================================================
void CBaseAnimating::ResetSequenceInfo ( )
{
	if (m_nSequence == -1)
	{
		// This shouldn't happen.  Setting m_nSequence blindly is a horrible coding practice.
		m_nSequence = 0;
	}

	m_flGroundSpeed = GetSequenceGroundSpeed( m_nSequence );
	m_bSequenceLoops = ((GetSequenceFlags( m_nSequence ) & STUDIO_LOOPING) != 0);
	// m_flAnimTime = gpGlobals->time;
	m_flPlaybackRate = 1.0;
	m_bSequenceFinished = false;
	m_flLastEventCheck = 0;
	m_nNewSequenceParity = ( ++m_nNewSequenceParity ) & EF_PARITY_MASK;
	m_nResetEventsParity = ( ++m_nResetEventsParity ) & EF_PARITY_MASK;
}

//=========================================================
//=========================================================
bool CBaseAnimating::IsValidSequence( int iSequence )
{
	Assert( GetModelPtr() );
	studiohdr_t* pstudiohdr = GetModelPtr( );
	if (iSequence < 0 || iSequence >= pstudiohdr->numseq)
	{
		return false;
	}
	return true;
}


//=========================================================
//=========================================================
int CBaseAnimating::GetSequenceFlags( int iSequence )
{
	Assert( GetModelPtr() );
	return ::GetSequenceFlags( GetModelPtr(), iSequence );
}

//=========================================================
//=========================================================
float CBaseAnimating::SequenceDuration( int iSequence )
{
//	Assert( iSequence != -1 );
	studiohdr_t* pstudiohdr = GetModelPtr( );
	if ( !pstudiohdr )
	{
		DevWarning( 2, "CBaseAnimating::SequenceDuration( %d ) NULL pstudiohdr on %s!\n", iSequence, GetClassname() );
		return 0.1;
	}
	if (iSequence >= pstudiohdr->numseq || iSequence < 0 )
	{
		DevWarning( 2, "CBaseAnimating::SequenceDuration( %d ) out of range\n", iSequence );
		return 0.1;
	}

	return Studio_Duration( pstudiohdr, iSequence, GetPoseParameterArray() );
}

float CBaseAnimating::GetSequenceCycleRate( int iSequence )
{
	float t = SequenceDuration( iSequence );

	if (t > 0.0f)
	{
		return 1.0f / t;
	}
	else
	{
		return 1.0f / 0.1f;
	}
}


float CBaseAnimating::GetLastVisibleCycle( int iSequence )
{
	studiohdr_t* pstudiohdr = GetModelPtr( );
	if ( !pstudiohdr )
	{
		DevWarning( 2, "CBaseAnimating::LastVisibleCycle( %d ) NULL pstudiohdr on %s!\n", iSequence, GetClassname() );
		return 1.0;
	}

	if (!(GetSequenceFlags( iSequence ) & STUDIO_LOOPING))
	{
		return 1.0f - (pstudiohdr->pSeqdesc( iSequence )->fadeouttime) * GetSequenceCycleRate( iSequence ) * m_flPlaybackRate;
	}
	else
	{
		return 1.0;
	}
}


float CBaseAnimating::GetSequenceGroundSpeed( int iSequence )
{
	float t = SequenceDuration( iSequence );

	if (t > 0)
	{
		return GetSequenceMoveDist( iSequence ) / t;
	}
	else
	{
		return 0;
	}
}

float CBaseAnimating::GetIdealSpeed( ) const
{
	return m_flGroundSpeed;
}


//-----------------------------------------------------------------------------
// Purpose: Returns true if the given sequence has the anim event, false if not.
// Input  : nSequence - sequence number to check
//			nEvent - anim event number to look for
//-----------------------------------------------------------------------------
bool CBaseAnimating::HasAnimEvent( int nSequence, int nEvent )
{
	studiohdr_t *pstudiohdr = GetModelPtr();
	if ( !pstudiohdr )
	{
		return false;
	}

  	animevent_t event;

	int index = 0;
	while ( ( index = GetAnimationEvent( pstudiohdr, nSequence, &event, 0.0f, 1.0f, index ) ) != 0 )
	{
		if ( event.event == nEvent )
		{
			return true;
		}
	}

	return false;
}


//=========================================================
// DispatchAnimEvents
//=========================================================
void CBaseAnimating::DispatchAnimEvents ( CBaseAnimating *eventHandler )
{
	// don't fire events if the framerate is 0
	if (m_flPlaybackRate == 0.0)
		return;

  	animevent_t	event;

	studiohdr_t *pstudiohdr = GetModelPtr( );

	if ( !pstudiohdr )
	{
	//	Assert(!"CBaseAnimating::DispatchAnimEvents: model missing"); // VXP
		return;
	}

	// skip this altogether if there are no events
	if (pstudiohdr->pSeqdesc( m_nSequence )->numevents == 0)
	{
		return;
	}

	// look from when it last checked to some short time in the future	
	float flCycleRate = GetSequenceCycleRate( m_nSequence ) * m_flPlaybackRate;
	float flStart = m_flLastEventCheck;
	float flEnd = m_flCycle;

	if (!m_bSequenceLoops && m_bSequenceFinished)
	{
	//	flEnd = 1.0f;
		flEnd = 1.01f; // VXP: From Source 2007 - magic number? 0_o
	}
	m_flLastEventCheck = flEnd;

	/*
	if (m_debugOverlays & OVERLAY_NPC_SELECTED_BIT)
	{
		Msg( "%s:%s : checking %.2f %.2f (%d)\n", STRING(GetModelName()), pstudiohdr->pSeqdesc( m_nSequence )->pszLabel(), flStart, flEnd, m_bSequenceFinished );
	}
	*/

	// FIXME: does not handle negative framerates!
	int index = 0;
	while ( (index = GetAnimationEvent( pstudiohdr, m_nSequence, &event, flStart, flEnd, index ) ) != 0 )
	{
		event.pSource = this;
		// calc when this event should happen
	//	event.eventtime = m_flAnimTime + (event.cycle - m_flCycle) / flCycleRate;
		if (flCycleRate > 0.0)
		{
			float flCycle = event.cycle;
			if (flCycle > m_flCycle)
			{
				flCycle = flCycle - 1.0;
			}
			event.eventtime = m_flAnimTime + (flCycle - m_flCycle) / flCycleRate + GetAnimTimeInterval();
		}

		/*
		if (m_debugOverlays & OVERLAY_NPC_SELECTED_BIT)
		{
			Msg( "dispatch %i (%i) cycle %f event cycle %f cyclerate %f\n", 
				(int)(index - 1), 
				(int)event.event, 
				(float)m_flCycle, 
				(float)event.cycle, 
				(float)flCycleRate );
		}
		*/
		eventHandler->HandleAnimEvent( &event );
	}
}


// SetPoseParamater()

//=========================================================
//=========================================================
float CBaseAnimating::SetPoseParameter( const char *szName, float flValue )
{
	return SetPoseParameter( LookupPoseParameter( szName ), flValue );
}

float CBaseAnimating::SetPoseParameter( int iParameter, float flValue )
{
	studiohdr_t *pstudiohdr = GetModelPtr( );

	if ( !pstudiohdr )
	{
		Assert(!"CBaseAnimating::SetPoseParameter: model missing");
		return flValue;
	}

	if (iParameter >= 0)
	{
		float flNewValue;
		flValue = Studio_SetPoseParameter( pstudiohdr, iParameter, flValue, flNewValue );
		m_flPoseParameter.Set( iParameter, flNewValue );
	}

	return flValue;
}

//=========================================================
//=========================================================
float CBaseAnimating::GetPoseParameter( const char *szName )
{
	return GetPoseParameter( LookupPoseParameter( szName ) );
}

float CBaseAnimating::GetPoseParameter( int iParameter )
{
	studiohdr_t *pstudiohdr = GetModelPtr( );

	if ( !pstudiohdr )
	{
		Assert(!"CBaseAnimating::GetPoseParameter: model missing");
		return 0.0;
	}

	if (iParameter >= 0)
	{
		return Studio_GetPoseParameter( pstudiohdr, iParameter, m_flPoseParameter[ iParameter ] );
	}

	return 0.0;
}

//=========================================================
//=========================================================
int CBaseAnimating::LookupPoseParameter( const char *szName )
{
	studiohdr_t *pstudiohdr = GetModelPtr( );

	if ( !pstudiohdr )
	{
		return 0;
	}

	for (int i = 0; i < pstudiohdr->numposeparameters; i++)
	{
		if (stricmp( pstudiohdr->pPoseParameter( i )->pszName(), szName ) == 0)
		{
			return i;
		}
	}

	// AssertMsg( 0, UTIL_VarArgs( "poseparameter %s couldn't be mapped!!!\n", szName ) );
	return -1; // Error
}

//=========================================================
//=========================================================
bool CBaseAnimating::HasPoseParameter( int iSequence, const char *szName )
{
	int iParameter = LookupPoseParameter( szName );
	if (iParameter == -1)
	{
		return false;
	}

	return HasPoseParameter( iSequence, iParameter );
}

//=========================================================
//=========================================================
bool CBaseAnimating::HasPoseParameter( int iSequence, int iParameter )
{
	studiohdr_t *pstudiohdr = GetModelPtr( );

	if ( !pstudiohdr )
	{
		return false;
	}

	if (iSequence < 0 || iSequence >= pstudiohdr->numseq)
	{
		return false;
	}

	mstudioseqdesc_t *pSeqDesc = pstudiohdr->pSeqdesc( iSequence );

	if (pSeqDesc->paramindex[0] == iParameter || pSeqDesc->paramindex[1] == iParameter)
	{
		return true;
	}
	return false;
}

//-----------------------------------------------------------------------------
// Purpose: Returns index number of a given named bone
// Input  : name of a bone
// Output :	Bone index number or -1 if bone not found
//-----------------------------------------------------------------------------
int CBaseAnimating::LookupBone( const char *szName )
{
	Assert( GetModelPtr() );

	return Studio_BoneIndexByName( GetModelPtr(), szName );
}


//=========================================================
//=========================================================
void CBaseAnimating::GetBonePosition ( int iBone, Vector &origin, QAngle &angles )
{
	studiohdr_t *pStudioHdr = GetModelPtr( );
	if (!pStudioHdr)
	{
		Assert(!"CBaseAnimating::GetBonePosition: model missing");
		return;
	}

	if (iBone < 0 || iBone >= pStudioHdr->numbones)
	{
		Assert(!"CBaseAnimating::GetBonePosition: invalid bone index");
		return;
	}

	matrix3x4_t bonetoworld;
	GetBoneTransform( iBone, bonetoworld );
	
	MatrixAngles( bonetoworld, angles, origin );
}



//=========================================================
//=========================================================

void CBaseAnimating::GetBoneTransform( int iBone, matrix3x4_t &pBoneToWorld )
{
	studiohdr_t *pStudioHdr = GetModelPtr( );

	if (!pStudioHdr)
	{
		Assert(!"CBaseAnimating::GetBoneTransform: model missing");
		return;
	}

	if (iBone < 0 || iBone >= pStudioHdr->numbones)
	{
		Assert(!"CBaseAnimating::GetBoneTransform: invalid bone index");
		return;
	}

	studiocache_t *pcache = GetBoneCache( );

	matrix3x4_t *pmatrix = Studio_LookupCachedBone( pcache, iBone );

	if ( !pmatrix )
	{
		MatrixCopy( EntityToWorldTransform(), pBoneToWorld );
		return;
	}

	Assert( pmatrix );
	
	// FIXME
	MatrixCopy( *pmatrix, pBoneToWorld );
}

void CBaseAnimating::CalculateIKLocks( float currentTime )
{
	if ( m_pIk )
	{
		Vector up;
		GetVectors( NULL, NULL, &up );
		// FIXME: check number of slots?
		for (int i = 0; i < m_pIk->m_target.Count(); i++)
		{
			trace_t trace;
			iktarget_t *pTarget = &m_pIk->m_target[i];

			if (pTarget->est.time != currentTime)
				continue;

			Vector p1, p2;
			VectorMA( pTarget->est.pos, pTarget->est.height, up, p1 );
			VectorMA( pTarget->est.pos, -pTarget->est.height, up, p2 );

			float r = pTarget->est.radius;

			// don't IK to other characters
			UTIL_TraceHull( p1, p2, Vector(-r,-r,0), Vector(r,r,1), (MASK_SOLID & ~CONTENTS_MONSTER), this, COLLISION_GROUP_NONE, &trace );
			pTarget->est.pos = trace.endpos;
		}
	}
}

void CBaseAnimating::SetupBones( matrix3x4_t *pBoneToWorld, int boneMask )
{
	Assert( GetModelPtr() );

	studiohdr_t *pStudioHdr = GetModelPtr( );

	Vector pos[MAXSTUDIOBONES];
	Quaternion q[MAXSTUDIOBONES];

	if ( m_pIk )
	{
		m_pIk->Init( pStudioHdr, GetAbsAngles(), GetAbsOrigin(), gpGlobals->curtime );
		GetSkeleton( pos, q, boneMask );

		CalculateIKLocks( gpGlobals->curtime );
		m_pIk->SolveDependencies( pos, q );
	}
	else
	{
		GetSkeleton( pos, q, boneMask );
	}


	Studio_BuildMatrices( 
		pStudioHdr, 
		GetAbsAngles(), 
		GetAbsOrigin(), 
		pos, 
		q, 
		-1,
		pBoneToWorld,
		boneMask );
}

//=========================================================
//=========================================================
int CBaseAnimating::GetNumBones ( void )
{
	studiohdr_t *pStudioHdr = GetModelPtr( );
	if(pStudioHdr)
	{
		return pStudioHdr->numbones;
	}
	else
	{
		Assert(!"CBaseAnimating::GetNumBones: model missing");
		return 0;
	}
}


//=========================================================
//=========================================================

//-----------------------------------------------------------------------------
// Purpose: Returns index number of a given named attachment
// Input  : name of attachment
// Output :	attachment index number or -1 if attachment not found
//-----------------------------------------------------------------------------
int CBaseAnimating::LookupAttachment( const char *szName )
{
	studiohdr_t *pStudioHdr = GetModelPtr( );
	if (!pStudioHdr)
	{
		Assert(!"CBaseAnimating::LookupAttachment: model missing");
		return 0;
	}

	// The +1 is to make attachment indices be 1-based (namely 0 == invalid or unused attachment)
	return Studio_FindAttachment( pStudioHdr, szName ) + 1;
}


//-----------------------------------------------------------------------------
// Purpose: Returns the world location and world angles of an attachment
// Input  : attachment name
// Output :	location and angles
//-----------------------------------------------------------------------------
bool CBaseAnimating::GetAttachment( const char *szName, Vector &absOrigin, QAngle &absAngles )
{																
	return GetAttachment( LookupAttachment( szName ), absOrigin, absAngles );
}

//-----------------------------------------------------------------------------
// Purpose: Returns the world location and world angles of an attachment
// Input  : attachment index
// Output :	location and angles
//-----------------------------------------------------------------------------
bool CBaseAnimating::GetAttachment ( int iAttachment, Vector &absOrigin, QAngle &absAngles )
{
	matrix3x4_t attachmentToWorld;

	if (GetAttachment( iAttachment, attachmentToWorld ))
	{
		MatrixAngles( attachmentToWorld, absAngles, absOrigin );
		return true;
	}

	absOrigin.Init();
	absAngles.Init();
	return false;
}


//-----------------------------------------------------------------------------
// Purpose: Returns the world location and world angles of an attachment
// Input  : attachment index
// Output :	location and angles
//-----------------------------------------------------------------------------
bool CBaseAnimating::GetAttachment ( int iAttachment, matrix3x4_t &attachmentToWorld )
{
	studiohdr_t *pStudioHdr = GetModelPtr( );
	if (!pStudioHdr)
	{
		Assert(!"CBaseAnimating::GetAttachment: model missing");
		return false;
	}

	if(iAttachment < 1 || iAttachment > pStudioHdr->numattachments)
	{
//		Assert(!"CBaseAnimating::GetAttachment: invalid attachment index");
		return false;
	}

	mstudioattachment_t *pattachment = pStudioHdr->pAttachment( iAttachment-1 );

	matrix3x4_t bonetoworld;
	GetBoneTransform( pattachment->bone, bonetoworld );
	ConcatTransforms( bonetoworld, pattachment->local, attachmentToWorld ); 

	return true;
}


//-----------------------------------------------------------------------------
// Purpose: Returns the world location of an attachment
// Input  : attachment index
// Output :	location and angles
//-----------------------------------------------------------------------------
bool CBaseAnimating::GetAttachment( const char *szName, Vector &absOrigin, Vector *forward, Vector *right, Vector *up )
{																
	return GetAttachment( LookupAttachment( szName ), absOrigin, forward, right, up );
}

bool CBaseAnimating::GetAttachment( int iAttachment, Vector &absOrigin, Vector *forward, Vector *right, Vector *up )
{
	matrix3x4_t attachmentToWorld;

	if (GetAttachment( iAttachment, attachmentToWorld ))
	{
		MatrixPosition( attachmentToWorld, absOrigin );
		if (forward)
		{
			MatrixGetColumn( attachmentToWorld, 0, forward );
		}
		if (right)
		{
			MatrixGetColumn( attachmentToWorld, 1, right );
		}
		if (up)
		{
			MatrixGetColumn( attachmentToWorld, 2, up );
		}
		return true;
	}

	absOrigin.Init();

	if (forward)
	{
		forward->Init();
	}
	if (right)
	{
		right->Init();
	}
	if (up)
	{
		up->Init();
	}
	return false;
}


//-----------------------------------------------------------------------------
// Returns the attachment in local space
//-----------------------------------------------------------------------------
bool CBaseAnimating::GetAttachmentLocal( const char *szName, Vector &origin, QAngle &angles )
{
	return GetAttachmentLocal( LookupAttachment( szName ), origin, angles );
}

bool CBaseAnimating::GetAttachmentLocal( int iAttachment, Vector &origin, QAngle &angles )
{
	matrix3x4_t attachmentToEntity;

	if (GetAttachmentLocal( iAttachment, attachmentToEntity ))
	{
		MatrixAngles( attachmentToEntity, angles, origin );
		return true;
	}
	return false;
}

bool CBaseAnimating::GetAttachmentLocal( int iAttachment, matrix3x4_t &attachmentToLocal )
{
	matrix3x4_t attachmentToWorld;
	if (!GetAttachment(iAttachment, attachmentToWorld))
		return false;

	matrix3x4_t worldToEntity;
	MatrixInvert( EntityToWorldTransform(), worldToEntity );
	ConcatTransforms( worldToEntity, attachmentToWorld, attachmentToLocal ); 
	return true;
}


//=========================================================
//=========================================================
void CBaseAnimating::GetEyeballs( Vector &origin, QAngle &angles )
{
	studiohdr_t *pStudioHdr = GetModelPtr( );
	if (!pStudioHdr)
	{
		Assert(!"CBaseAnimating::GetAttachment: model missing");
		return;
	}

	for (int iBodypart = 0; iBodypart < pStudioHdr->numbodyparts; iBodypart++)
	{
		mstudiobodyparts_t *pBodypart = pStudioHdr->pBodypart( iBodypart );
		for (int iModel = 0; iModel < pBodypart->nummodels; iModel++)
		{
			mstudiomodel_t *pModel = pBodypart->pModel( iModel );
			for (int iEyeball = 0; iEyeball < pModel->numeyeballs; iEyeball++)
			{
				mstudioeyeball_t *pEyeball = pModel->pEyeball( iEyeball );
				matrix3x4_t bonetoworld;
				GetBoneTransform( pEyeball->bone, bonetoworld );
				VectorTransform( pEyeball->org, bonetoworld,  origin );
				MatrixAngles( bonetoworld, angles ); // ???
			}
		}
	}
}


//=========================================================
//=========================================================
int CBaseAnimating::FindTransitionSequence( int iCurrentSequence, int iGoalSequence, int *piDir )
{
	Assert( GetModelPtr() );

	if (piDir == NULL)
	{
		int iDir = 1;
		int sequence = ::FindTransitionSequence( GetModelPtr(), iCurrentSequence, iGoalSequence, &iDir );
		if (iDir != 1)
			return -1;
		else
			return sequence;
	}

	return ::FindTransitionSequence( GetModelPtr(), iCurrentSequence, iGoalSequence, piDir );
}



int CBaseAnimating::GetEntryNode( int iSequence )
{
	studiohdr_t *pstudiohdr = GetModelPtr();
	if (! pstudiohdr)
		return 0;
	
	mstudioseqdesc_t *pseqdesc = pstudiohdr->pSeqdesc( iSequence );

	return pseqdesc->entrynode;
}


int CBaseAnimating::GetExitNode( int iSequence )
{
	studiohdr_t *pstudiohdr = GetModelPtr();
	if (! pstudiohdr)
		return 0;
	
	mstudioseqdesc_t *pseqdesc = pstudiohdr->pSeqdesc( iSequence );

	return pseqdesc->exitnode;
}


float CBaseAnimating::GetExitPhase( int iSequence )
{
	studiohdr_t *pstudiohdr = GetModelPtr();
	if (! pstudiohdr)
		return 0;
	
	mstudioseqdesc_t *pseqdesc = pstudiohdr->pSeqdesc( iSequence );

	return pseqdesc->exitphase;
}

//=========================================================
//=========================================================

void CBaseAnimating::SetBodygroup( int iGroup, int iValue )
{
	Assert( GetModelPtr() );

	int newBody = m_nBody;
	::SetBodygroup( GetModelPtr( ), newBody, iGroup, iValue );
	m_nBody = newBody;
}

int CBaseAnimating::GetBodygroup( int iGroup )
{
	Assert( GetModelPtr() );

	return ::GetBodygroup( GetModelPtr( ), m_nBody, iGroup );
}

const char *CBaseAnimating::GetBodygroupName( int iGroup )
{
	Assert( GetModelPtr() );

	return ::GetBodygroupName( GetModelPtr( ), iGroup );
}

int CBaseAnimating::FindBodygroupByName( const char *name )
{
	Assert( GetModelPtr() );

	return ::FindBodygroupByName( GetModelPtr( ), name );
}

int CBaseAnimating::GetBodygroupCount( int iGroup )
{
	Assert( GetModelPtr() );

	return ::GetBodygroupCount( GetModelPtr( ), iGroup );
}

int CBaseAnimating::GetNumBodyGroups( void )
{
	Assert( GetModelPtr() );

	return ::GetNumBodyGroups( GetModelPtr( ) );
}

int CBaseAnimating::ExtractBbox( int sequence, Vector& mins, Vector& maxs )
{
	Assert( GetModelPtr() );

	return ::ExtractBbox( GetModelPtr( ), sequence, mins, maxs );
}

//=========================================================
//=========================================================

void CBaseAnimating::SetSequenceBox( void )
{
	Vector mins, maxs;

	// Get sequence bbox
	if ( ExtractBbox( m_nSequence, mins, maxs ) )
	{
		// expand box for rotation
		// find min / max for rotations
		float yaw = GetLocalAngles().y * (M_PI / 180.0);
		
		Vector xvector, yvector;
		xvector.x = cos(yaw);
		xvector.y = sin(yaw);
		yvector.x = -sin(yaw);
		yvector.y = cos(yaw);
		Vector bounds[2];

		bounds[0] = mins;
		bounds[1] = maxs;
		
		Vector rmin( 9999, 9999, 9999 );
		Vector rmax( -9999, -9999, -9999 );
		Vector base, transformed;

		for (int i = 0; i <= 1; i++ )
		{
			base.x = bounds[i].x;
			for ( int j = 0; j <= 1; j++ )
			{
				base.y = bounds[j].y;
				for ( int k = 0; k <= 1; k++ )
				{
					base.z = bounds[k].z;
					
				// transform the point
					transformed.x = xvector.x*base.x + yvector.x*base.y;
					transformed.y = xvector.y*base.x + yvector.y*base.y;
					transformed.z = base.z;
					
					for ( int l = 0; l < 3; l++ )
					{
						if (transformed[l] < rmin[l])
							rmin[l] = transformed[l];
						if (transformed[l] > rmax[l])
							rmax[l] = transformed[l];
					}
				}
			}
		}
		rmin.z = 0;
		rmax.z = rmin.z + 1;
		UTIL_SetSize( this, rmin, rmax );
	}
}

//=========================================================
//=========================================================
int CBaseAnimating::RegisterPrivateActivity( const char *pszActivityName )
{
	return ActivityList_RegisterPrivateActivity( pszActivityName );
}

//-----------------------------------------------------------------------------
// Purpose: Notifies the console that this entity could not retrieve an
//			animation sequence for the specified activity. This probably means
//			there's a typo in the model QC file, or the sequence is missing
//			entirely.
//			
//
// Input  : iActivity - The activity that failed to resolve to a sequence.
//
//
// NOTE   :	IMPORTANT - Something needs to be done so that private activities
//			(which are allowed to collide in the activity list) remember each
//			entity that registered an activity there, and the activity name
//			each character registered.
//-----------------------------------------------------------------------------
void CBaseAnimating::ReportMissingActivity( int iActivity )
{
	Msg( "%s has no sequence for act:%s\n", GetClassname(), ActivityList_NameForIndex(iActivity) );
}


int CBaseAnimating::GetNumFlexControllers( void )
{
	studiohdr_t *pstudiohdr = GetModelPtr( );
	if (! pstudiohdr)
		return 0;

	return pstudiohdr->numflexcontrollers;
}


const char *CBaseAnimating::GetFlexDescFacs( int iFlexDesc )
{
	studiohdr_t *pstudiohdr = GetModelPtr( );
	if (! pstudiohdr)
		return 0;

	mstudioflexdesc_t *pflexdesc = pstudiohdr->pFlexdesc( iFlexDesc );

	return pflexdesc->pszFACS( );
}

const char *CBaseAnimating::GetFlexControllerName( int iFlexController )
{
	studiohdr_t *pstudiohdr = GetModelPtr( );
	if (! pstudiohdr)
		return 0;

	mstudioflexcontroller_t *pflexcontroller = pstudiohdr->pFlexcontroller( iFlexController );

	return pflexcontroller->pszName( );
}

const char *CBaseAnimating::GetFlexControllerType( int iFlexController )
{
	studiohdr_t *pstudiohdr = GetModelPtr( );
	if (! pstudiohdr)
		return 0;

	mstudioflexcontroller_t *pflexcontroller = pstudiohdr->pFlexcontroller( iFlexController );

	return pflexcontroller->pszType( );
}

//-----------------------------------------------------------------------------
// Purpose: Converts the ground speed of the animating entity into a true velocity
// Output : Vector - velocity of the character at its current m_flGroundSpeed
//-----------------------------------------------------------------------------
Vector CBaseAnimating::GetGroundSpeedVelocity( void )
{
	studiohdr_t *pstudiohdr = GetModelPtr();
	if (!pstudiohdr)
		return vec3_origin;

	QAngle  vecAngles;
	Vector	vecVelocity;

	vecAngles.y = GetSequenceMoveYaw( m_nSequence );
	vecAngles.x = 0;
	vecAngles.z = 0;

	vecAngles.y += GetLocalAngles().y;

	AngleVectors( vecAngles, &vecVelocity );

	vecVelocity = vecVelocity * m_flGroundSpeed;

	return vecVelocity;
}


//-----------------------------------------------------------------------------
// Purpose:
// Output :
//-----------------------------------------------------------------------------
float CBaseAnimating::GetInstantaneousVelocity( float flInterval )
{
	studiohdr_t *pstudiohdr = GetModelPtr( );
	if (! pstudiohdr)
		return 0;

	// FIXME: someone needs to check for last frame, etc.
	float flNextCycle = m_flCycle + flInterval * GetSequenceCycleRate( m_nSequence ) * m_flPlaybackRate;

	Vector vecVelocity;
	Studio_SeqVelocity( pstudiohdr, m_nSequence, flNextCycle, GetPoseParameterArray(), vecVelocity );
	vecVelocity *= m_flPlaybackRate;

	return vecVelocity.Length();
}



//-----------------------------------------------------------------------------
// Purpose:
// Output :
//-----------------------------------------------------------------------------
bool CBaseAnimating::GetIntervalMovement( float flIntervalUsed, bool &bMoveSeqFinished, Vector &newPosition, QAngle &newAngles )
{
	studiohdr_t *pstudiohdr = GetModelPtr( );
	if (! pstudiohdr)
		return false;

	float flComputedCycleRate = GetSequenceCycleRate( m_nSequence );

	float flNextCycle = m_flCycle + flIntervalUsed * flComputedCycleRate * m_flPlaybackRate;

	if ((!m_bSequenceLoops) && flNextCycle > 1.0)
	{
		flIntervalUsed = m_flCycle / (flComputedCycleRate * m_flPlaybackRate);
		flNextCycle = 1.0;
		bMoveSeqFinished = true;
	}
	else
	{
		bMoveSeqFinished = false;
	}

	Vector deltaPos;
	QAngle deltaAngles;

	if (Studio_SeqMovement( pstudiohdr, m_nSequence, m_flCycle, flNextCycle, GetPoseParameterArray(), deltaPos, deltaAngles ))
	{
		VectorYawRotate( deltaPos, GetLocalAngles().y, deltaPos );
		newPosition = GetLocalOrigin() + deltaPos;
		newAngles.Init();
		newAngles.y = GetLocalAngles().y + deltaAngles.y;
		return true;
	}
	else
	{
		newPosition = GetLocalOrigin();
		newAngles = GetLocalAngles();
		return false;
	}
}



//-----------------------------------------------------------------------------
// Purpose: find frame where they animation has moved a given distance.
// Output :
//-----------------------------------------------------------------------------
float CBaseAnimating::GetMovementFrame( float flDist )
{
	studiohdr_t *pstudiohdr = GetModelPtr( );
	if (! pstudiohdr)
		return 0;

	float t = Studio_FindSeqDistance( pstudiohdr, m_nSequence, GetPoseParameterArray(), flDist );

	return t;
}


//-----------------------------------------------------------------------------
// Purpose: does a specific sequence have movement?
// Output :
//-----------------------------------------------------------------------------
bool CBaseAnimating::HasMovement( int iSequence )
{
	studiohdr_t *pstudiohdr = GetModelPtr( );
	if (! pstudiohdr)
		return false;

	// FIXME: this needs to check to see if there are keys, and the object is walking
	Vector deltaPos;
	QAngle deltaAngles;
	if (Studio_SeqMovement( pstudiohdr, iSequence, 0.0f, 1.0f, GetPoseParameterArray(), deltaPos, deltaAngles ))
	{
		return true;
	}

	return false;
}



//-----------------------------------------------------------------------------
// Purpose: 
// Input  : *szModelName - 
//-----------------------------------------------------------------------------
void CBaseAnimating::SetModel( const char *szModelName )
{
	if ( szModelName[0] )
	{
		int modelIndex = modelinfo->GetModelIndex( szModelName );
		const model_t *model = modelinfo->GetModel( modelIndex );
		if ( model && ( modelinfo->GetModelType( model ) != mod_studio ) )
		{
			Msg( "Setting CBaseAnimating to non-studio model %s  (type:%i)\n",	szModelName, modelinfo->GetModelType( model ) );
		}
	}
	
	UTIL_SetModel( this, szModelName );
}

studiohdr_t *CBaseAnimating::GetModelPtr( void ) 
{ 
	model_t *model = GetModel();
	if ( !model )
		return NULL;

	// In TF2, certain base animating classes (gates, etc) can be mod_brush
	if ( modelinfo->GetModelType( model ) != mod_studio )
		return NULL;

	return static_cast< studiohdr_t * >( modelinfo->GetModelExtraData( model ) ); 
}

//-----------------------------------------------------------------------------
// Purpose: return the index to the shared bone cache
// Output :
//-----------------------------------------------------------------------------
studiocache_t *CBaseAnimating::GetBoneCache( void )
{
	studiohdr_t *pStudioHdr = GetModelPtr( );
	Assert(pStudioHdr);

	int boneMask = BONE_USED_BY_HITBOX | BONE_USED_BY_ATTACHMENT;

	studiocache_t *pcache = Studio_GetBoneCache( pStudioHdr, m_nSequence, m_flAnimTime, GetAbsAngles(), GetAbsOrigin(), boneMask );
	if ( !pcache )
	{
		// OPTIMIZE: Only setup bones that have hitboxes or have children with a hitbox
		matrix3x4_t bonetoworld[MAXSTUDIOBONES];
		SetupBones( bonetoworld, boneMask );

		pcache = Studio_SetBoneCache( pStudioHdr, m_nSequence, m_flAnimTime, GetAbsAngles(), GetAbsOrigin(), boneMask, bonetoworld );
	}
	Assert(pcache);
	return pcache;
}


void CBaseAnimating::InvalidateBoneCache( void )
{
	studiohdr_t *pStudioHdr = GetModelPtr( );
	Assert(pStudioHdr);

	int boneMask = BONE_USED_BY_HITBOX | BONE_USED_BY_ATTACHMENT;

	studiocache_t *pcache = Studio_GetBoneCache( pStudioHdr, m_nSequence, m_flAnimTime, GetAbsAngles(), GetAbsOrigin(), boneMask );

	if (pcache)
	{
		Studio_InvalidateBoneCache( pcache );
	}
}

bool CBaseAnimating::TestCollision( const Ray_t &ray, unsigned int fContentsMask, trace_t& tr )
{
	if ( ray.m_IsRay && IsSolidFlagSet( FSOLID_CUSTOMRAYTEST ))
	{
		if (!TestHitboxes( ray, fContentsMask, tr ))
			return true;

		return tr.DidHit();
	}

	if ( !ray.m_IsRay && IsSolidFlagSet( FSOLID_CUSTOMBOXTEST ))
	{
		// What do we do if we've got custom collisions but we're tracing a box against us?
		// Naturally, we throw up our hands, say we collided, but don't fill in trace info
		return true;
	}

	// We shouldn't get here.
	Assert(0);
	return false;
}

bool CBaseAnimating::TestHitboxes( const Ray_t &ray, unsigned int fContentsMask, trace_t& tr )
{
	studiohdr_t *pStudioHdr = GetModelPtr( );
	if (!pStudioHdr)
	{
		Assert(!"CBaseAnimating::GetBonePosition: model missing");
		return false;
	}

	mstudiohitboxset_t *set = pStudioHdr->pHitboxSet( m_nHitboxSet );
	if ( !set || !set->numhitboxes )
		return false;

	// Use vcollide for box traces
	if ( !ray.m_IsRay )
		return false;

	// This *has* to be true for the existing code to function correctly.
	Assert( ray.m_StartOffset == vec3_origin );

	studiocache_t *pcache = GetBoneCache( );

	matrix3x4_t *hitboxbones[MAXSTUDIOBONES];
	Studio_LinkHitboxCache( hitboxbones, pcache, pStudioHdr, set );

	if ( TraceToStudio( ray, pStudioHdr, set, hitboxbones, fContentsMask, tr ) )
	{
		mstudiobbox_t *pbox = set->pHitbox( tr.hitbox );
		mstudiobone_t *pBone = pStudioHdr->pBone(pbox->bone);
		tr.surface.name = "**studio**";
		tr.surface.flags = SURF_HITBOX;
		tr.surface.surfaceProps = physprops->GetSurfaceIndex( pBone->pszSurfaceProp() );
	}
	return true;
}

void CBaseAnimating::InitBoneControllers ( void ) // FIXME: rename
{
	int i;

	for (i = 0; i < NUM_BONECTRLS; i++)
	{
		SetBoneController( i, 0.0 );
	}

	for (i = 0; i < NUM_POSEPAREMETERS; i++)
	{
		SetPoseParameter( i, 0.0 );
	}
}

//=========================================================
//=========================================================
float CBaseAnimating::SetBoneController ( int iController, float flValue )
{
	Assert( GetModelPtr() );

	studiohdr_t *pmodel = (studiohdr_t*)GetModelPtr();

	Assert(iController >= 0 && iController < NUM_BONECTRLS);

	float newValue;
	float retVal = Studio_SetController( pmodel, iController, flValue, newValue );
	m_flEncodedController.Set( iController, newValue );

	return retVal;
}

//=========================================================
//=========================================================
float CBaseAnimating::GetBoneController ( int iController )
{
	Assert( GetModelPtr() );

	studiohdr_t *pmodel = (studiohdr_t*)GetModelPtr();

	return Studio_GetController( pmodel, iController, m_flEncodedController[iController] );
}

//------------------------------------------------------------------------------
// Purpose : Returns velcocity of the NPC from it's animation.  
//			 If physically simulated gets velocity from physics object
// Input   :
// Output  :
//------------------------------------------------------------------------------
void CBaseAnimating::GetVelocity(Vector *vVelocity, QAngle *vAngVelocity) 
{
	if ( GetMoveType() == MOVETYPE_VPHYSICS )
	{
		BaseClass::GetVelocity(vVelocity,vAngVelocity);
	}
	else if ( !(GetFlags() & FL_ONGROUND) )
	{
		BaseClass::GetVelocity(vVelocity,vAngVelocity);
	}
	else
	{
		if (vVelocity != NULL)
		{
			Vector	vRawVel;

			GetSequenceLinearMotion( m_nSequence,&vRawVel );

			// Build a rotation matrix from NPC orientation
			matrix3x4_t fRotateMatrix;
			AngleMatrix(GetLocalAngles(), fRotateMatrix);
			VectorRotate( vRawVel, fRotateMatrix, *vVelocity);
		}
		if (vAngVelocity != NULL)
		{
			*vAngVelocity	= GetLocalAngularVelocity();
		}
	}
}


//=========================================================
//=========================================================

void CBaseAnimating::GetSkeleton( Vector pos[], Quaternion q[], int boneMask )
{
	studiohdr_t *pStudioHdr = GetModelPtr();
	if(!pStudioHdr)
	{
		Assert(!"CBaseAnimating::GetSkeleton() without a model");
		return;
	}

	CalcPose( pStudioHdr, m_pIk, pos, q, m_nSequence, m_flCycle, GetPoseParameterArray(), boneMask );

	if ( m_pIk )
	{
		CIKContext auto_ik;
		auto_ik.Init( pStudioHdr, GetAbsAngles(), GetAbsOrigin(), gpGlobals->curtime );
		CalcAutoplaySequences( pStudioHdr, &auto_ik, pos, q, GetPoseParameterArray(), boneMask, gpGlobals->curtime );
	}
	else
	{
		CalcAutoplaySequences( pStudioHdr, NULL, pos, q, GetPoseParameterArray(), boneMask, gpGlobals->curtime );
	}
	CalcBoneAdj( pStudioHdr, pos, q, GetEncodedControllerArray(), boneMask );
}

int CBaseAnimating::DrawDebugTextOverlays(void) 
{
	int text_offset = BaseClass::DrawDebugTextOverlays();

	if (m_debugOverlays & OVERLAY_TEXT_BIT) 
	{
		// ----------------
		// Print Look time
		// ----------------
		char tempstr[1024];
		Q_snprintf(tempstr, sizeof(tempstr), "Model:%s", STRING(GetModelName()) );
		NDebugOverlay::EntityText(entindex(),text_offset,tempstr,0);
		text_offset++;
		Q_snprintf(tempstr, sizeof(tempstr), "Sequence: (%3d) %s",m_nSequence, GetSequenceName( m_nSequence ) );
		NDebugOverlay::EntityText(entindex(),text_offset,tempstr,0);
		text_offset++;
		const char *pActname = GetSequenceActivityName(m_nSequence);
		if ( pActname && strlen(pActname) )
		{
			Q_snprintf(tempstr, sizeof(tempstr), "Activity %s", pActname );
			NDebugOverlay::EntityText(entindex(),text_offset,tempstr,0);
			text_offset++;
		}

		Q_snprintf(tempstr, sizeof(tempstr), "Cycle: %.5f", (float)m_flCycle );
		NDebugOverlay::EntityText(entindex(),text_offset,tempstr,0);
		text_offset++;
	}
	return text_offset;
}

//-----------------------------------------------------------------------------
// Purpose: Force a clientside-animating entity to reset it's frame
//-----------------------------------------------------------------------------
void CBaseAnimating::ResetClientsideFrame( void )
{
	// TODO: Once we can chain MSG_ENTITY messages, use one of them
	m_bClientSideFrameReset = !(bool)m_bClientSideFrameReset;
	NetworkStateChanged();
}

//-----------------------------------------------------------------------------
// Purpose: Returns the origin at which to play an inputted dispatcheffect 
//-----------------------------------------------------------------------------
void CBaseAnimating::GetInputDispatchEffectPosition( const char *sInputString, Vector &pOrigin, QAngle &pAngles )
{
	// See if there's a specified attachment point
	int iAttachment;
	if ( GetModelPtr() && sscanf( sInputString, "%d", &iAttachment ) )
	{
		if ( !GetAttachment( iAttachment, pOrigin, pAngles ) )
		{
			Msg( "ERROR: Mapmaker tried to spawn DispatchEffect %s, but %s has no attachment %d\n", 
				sInputString, STRING(GetModelName()), iAttachment );
		}
		return;
	}

	BaseClass::GetInputDispatchEffectPosition( sInputString, pOrigin, pAngles );
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : setnum - 
//-----------------------------------------------------------------------------
void CBaseAnimating::SetHitboxSet( int setnum )
{
#ifdef _DEBUG
	studiohdr_t *pStudioHdr = GetModelPtr();
	if ( !pStudioHdr )
		return;

	if (setnum > pStudioHdr->numhitboxsets)
	{
		// Warn if an bogus hitbox set is being used....
		static bool s_bWarned = false;
		if (!s_bWarned)
		{
			Warning("Using bogus hitbox set in entity %s!\n", GetClassname() );
			s_bWarned = true;
		}
		setnum = 0;
	}
#endif

	m_nHitboxSet = setnum;
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : *setname - 
//-----------------------------------------------------------------------------
void CBaseAnimating::SetHitboxSetByName( const char *setname )
{
	Assert( GetModelPtr() );
	m_nHitboxSet = FindHitboxSetByName( GetModelPtr(), setname );
}

//-----------------------------------------------------------------------------
// Purpose: 
// Output : int
//-----------------------------------------------------------------------------
int CBaseAnimating::GetHitboxSet( void )
{
	return m_nHitboxSet;
}

//-----------------------------------------------------------------------------
// Purpose: 
// Output : char const
//-----------------------------------------------------------------------------
const char *CBaseAnimating::GetHitboxSetName( void )
{
	Assert( GetModelPtr() );
	return ::GetHitboxSetName( GetModelPtr(), m_nHitboxSet );
}

//-----------------------------------------------------------------------------
// Purpose: 
// Output : int
//-----------------------------------------------------------------------------
int CBaseAnimating::GetHitboxSetCount( void )
{
	Assert( GetModelPtr() );
	return ::GetHitboxSetCount( GetModelPtr() );
}

// For drawing server hitboxes via networking to client
static int boxpnt[6][4] = 
{
	{ 0, 4, 6, 2 }, // +X
	{ 0, 1, 5, 4 }, // +Y
	{ 0, 2, 3, 1 }, // +Z
	{ 7, 5, 1, 3 }, // -X
	{ 7, 3, 2, 6 }, // -Y
	{ 7, 6, 4, 5 }, // -Z
};	

static Vector	hullcolor[8] = 
{
	Vector( 1.0, 1.0, 1.0 ),
	Vector( 1.0, 0.5, 0.5 ),
	Vector( 0.5, 1.0, 0.5 ),
	Vector( 1.0, 1.0, 0.5 ),
	Vector( 0.5, 0.5, 1.0 ),
	Vector( 1.0, 0.5, 1.0 ),
	Vector( 0.5, 1.0, 1.0 ),
	Vector( 1.0, 1.0, 1.0 )
};

//-----------------------------------------------------------------------------
// Purpose: Send the current hitboxes for this model to the client ( to compare with
//  r_drawentities 3 client side boxes ).
// WARNING:  This uses a ton of bandwidth, only use on a listen server
//-----------------------------------------------------------------------------
void CBaseAnimating::DrawServerHitboxes( void )
{
	studiohdr_t *pStudioHdr = GetModelPtr();
	if ( !pStudioHdr )
		return;

	mstudiohitboxset_t *set =pStudioHdr->pHitboxSet( m_nHitboxSet );
	if ( !set )
		return;

	Vector position;
	QAngle angles;

	for ( int b = 0; b < set->numhitboxes; b++ )
	{
		mstudiobbox_t *pbox = set->pHitbox( b );

		GetBonePosition( pbox->bone, position, angles );

		int j = (pbox->group % 8);

		int r, g, b;

		r = ( int ) ( 255.0f * hullcolor[j][0] );
		g = ( int ) ( 255.0f * hullcolor[j][1] );
		b = ( int ) ( 255.0f * hullcolor[j][2] );

		NDebugOverlay::BoxAngles( position, pbox->bbmin, pbox->bbmax, angles, r, g, b, 0 ,0);
	}
}

int CBaseAnimating::GetHitboxBone( int hitboxIndex )
{
	studiohdr_t *pStudioHdr = GetModelPtr();
	if ( pStudioHdr )
	{
		mstudiohitboxset_t *set =pStudioHdr->pHitboxSet( m_nHitboxSet );
		if ( set && hitboxIndex < set->numhitboxes )
		{
			return set->pHitbox( hitboxIndex )->bone;
		}
	}
	return 0;
}


int CBaseAnimating::GetPhysicsBone( int boneIndex )
{
	studiohdr_t *pStudioHdr = GetModelPtr();
	if ( pStudioHdr )
	{
		if ( boneIndex >= 0 && boneIndex < pStudioHdr->numbones )
			return pStudioHdr->pBone( boneIndex )->physicsbone;
	}
	return 0;
}

bool CBaseAnimating::LookupHitbox( const char *szName, int& outSet, int& outBox )
{
	studiohdr_t* pHdr = GetModelPtr();

	outSet = -1;
	outBox = -1;

	if( !pHdr )
		return false;

	for( int set=0; set < pHdr->numhitboxsets; set++ )
	{
		for( int i = 0; i < pHdr->iHitboxCount(set); i++ )
		{
			mstudiobbox_t* pBox = pHdr->pHitbox( i, set );
			
			if( !pBox )
				continue;
			
			const char* szBoxName = pBox->pszHitboxName(/*pHdr*/);
			if( Q_stricmp( szBoxName, szName ) == 0 )
			{
				outSet = set;
				outBox = i;
				return true;
			}
		}
	}

	return false;
}

void CBaseAnimating::CopyAnimationDataFrom( CBaseAnimating *pSource )
{
	this->SetModelName( pSource->GetModelName() );
	this->SetModelIndex( pSource->GetModelIndex() );
	this->m_flCycle	= pSource->m_flCycle;
	this->m_fEffects = pSource->m_fEffects | EF_NOINTERP;
	this->m_nSequence = pSource->m_nSequence;
	this->m_flAnimTime = pSource->m_flAnimTime;
	this->m_nBody = pSource->m_nBody;
	this->m_nSkin = pSource->m_nSkin;
}

int CBaseAnimating::GetHitboxesFrontside( int *boxList, int boxMax, const Vector &normal, float dist )
{
	int count = 0;
	studiohdr_t *pStudioHdr = GetModelPtr();
	if ( pStudioHdr )
	{
		mstudiohitboxset_t *set = pStudioHdr->pHitboxSet( m_nHitboxSet );
		if ( set )
		{
			matrix3x4_t matrix;
			for ( int b = 0; b < set->numhitboxes; b++ )
			{
				mstudiobbox_t *pbox = set->pHitbox( b );

				GetBoneTransform( pbox->bone, matrix );
				Vector center = (pbox->bbmax + pbox->bbmin) * 0.5;
				Vector centerWs;
				VectorTransform( center, matrix, centerWs );
				if ( DotProduct( centerWs, normal ) >= dist )
				{
					if ( count < boxMax )
					{
						boxList[count] = b;
						count++;
					}
				}
			}
		}
	}

	return count;
}

void CBaseAnimating::EnableServerIK()
{
	if (!m_pIk)
	{
		m_pIk = new CIKContext;
	}
}

void CBaseAnimating::DisableServerIK()
{
	delete m_pIk;
	m_pIk = NULL;
}

Activity CBaseAnimating::GetSequenceActivity( int iSequence )
{
	if( iSequence == -1 )
	{
		return ACT_INVALID;
	}

	if ( !GetModelPtr() )
		return ACT_INVALID;

	return (Activity)::GetSequenceActivity( GetModelPtr(), iSequence );
}

void CBaseAnimating::ModifyOrAppendCriteria( AI_CriteriaSet& set )
{
	BaseClass::ModifyOrAppendCriteria( set );

	// TODO
	// Append any animation state parameters here
}

BEGIN_PREDICTION_DATA_NO_BASE( CBaseAnimating )

	DEFINE_FIELD( CBaseAnimating, m_nSkin, FIELD_INTEGER ),
	DEFINE_FIELD( CBaseAnimating, m_flGroundSpeed, FIELD_FLOAT ),
	DEFINE_FIELD( CBaseAnimating, m_flLastEventCheck, FIELD_FLOAT ),
	DEFINE_FIELD( CBaseAnimating, m_bSequenceFinished, FIELD_BOOLEAN ),
	DEFINE_FIELD( CBaseAnimating, m_bSequenceLoops, FIELD_BOOLEAN ),
	DEFINE_FIELD( CBaseAnimating, m_nForceBone, FIELD_INTEGER ),
	DEFINE_FIELD( CBaseAnimating, m_vecForce, FIELD_VECTOR ),
	DEFINE_FIELD( CBaseAnimating, m_nBody, FIELD_INTEGER ),
	DEFINE_FIELD( CBaseAnimating, m_nHitboxSet, FIELD_INTEGER ),
	DEFINE_FIELD( CBaseAnimating, m_flModelScale, FIELD_FLOAT ),
	DEFINE_ARRAY( CBaseAnimating, m_flPoseParameter, FIELD_FLOAT, CBaseAnimating::NUM_POSEPAREMETERS ),
	DEFINE_FIELD( CBaseAnimating, m_nSequence, FIELD_INTEGER ),
	DEFINE_FIELD( CBaseAnimating, m_flPlaybackRate, FIELD_FLOAT ),
	DEFINE_FIELD( CBaseAnimating, m_flCycle, FIELD_FLOAT ),
	DEFINE_ARRAY( CBaseAnimating, m_flEncodedController, FIELD_FLOAT, CBaseAnimating::NUM_BONECTRLS ),
	DEFINE_FIELD( CBaseAnimating, m_bClientSideAnimation, FIELD_BOOLEAN ),
	DEFINE_FIELD( CBaseAnimating, m_bClientSideFrameReset, FIELD_BOOLEAN ),

END_PREDICTION_DATA()
