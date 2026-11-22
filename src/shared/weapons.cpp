#include "weapons.h"

#include "shared.h"
#include "cod2_common.h"
#include "cod2_cmd.h"
#include "cod2_math.h"
#include "cod2_entity.h"
#include "cod2_dvars.h"
#include "cod2_player.h"
#include "tickrate_patch.h"
#include <math.h>
#include <string.h>
#include <vector>


// Toggle for custom pellet distribution (true) vs original random spread (false)
dvar_t* g_shotgun_spread_fix;
// 125 Hz fork: whether the fixed patterns are turned by a random angle on every shot (true, as CoD2x does) or always fired upright (false)
dvar_t* g_shotgun_spread_rotation;



// 125 Hz fork: walk speed scale from pulling the pin of a grenade to the end of the throw, 0 = the speed of the weapon in hand (original)
dvar_t* g_grenadeMoveSpeedScale;



struct WeaponDef
{
    char padding1[0x1dc];
    int shotCount;          // offset 0x1dc
    char padding2[0x84];
    float moveSpeedScale;   // offset 0x264
    char padding3[0x334];
    float minDamageRange;   // offset 0x59c
};
static_assert(offsetof(WeaponDef, shotCount) == WL(0x1dc, 0x1dc));
static_assert(offsetof(WeaponDef, moveSpeedScale) == WL(0x264, 0x264));
static_assert(offsetof(WeaponDef, minDamageRange) == WL(0x59c, 0x59c));
static_assert(offsetof(playerState_t, weapon) == 0xd4);
static_assert(offsetof(playerState_t, weaponstate) == 0xd8);

// Weapon definitions by weapon index, the array BG_GetWeaponDef reads
#define bg_weaponDefs ((WeaponDef**)ADDR(0x01728200, 0x086270a0))

// Weapon states of a grenade throw (offhand), set by PM_Weapon_CheckForOffHand and the functions it continues with
#define WEAPON_OFFHAND_INIT     12 // lowering the weapon
#define WEAPON_OFFHAND_PREPARE  13 // pulling the pin
#define WEAPON_OFFHAND_HOLD     14 // holding the grenade (cooking)
#define WEAPON_OFFHAND_START    15 // throwing
#define WEAPON_OFFHAND_END      16 // end of the throw, then the weapon is raised again

struct weaponParms
{
    float forward[3];
    float right[3];
    float up[3];
    float muzzleTrace[3];
    float gunForward[3];
    WeaponDef* weapDef;
};



// ===========================================================================================
// Multi-Circle Pellet Distribution
// ===========================================================================================

/**
 * Unified hardcoded 8-pellet pattern function with selectable variant and rotation.
 * variant == 0: original pattern (5 outer + 3 inner)
 * variant == 1: alternate pattern (4-cardinal outer + 4-diagonal inner)
 */
void CalculatePelletPosition_8Pellets(int pelletIdx, float rotationAngle, int variant, float* outX, float* outY)
{
    static const float pattern0[8][2] = {
        {  0.000f,  -0.800f },
        {  0.000f,   0.000f },
        {  0.000f,   0.400f },
        {  0.000f,  -0.400f },
        { -0.400f,   0.000f },
        {  0.400f,   0.000f },
        { -0.600f,   0.600f },
        {  0.600f,   0.600f }
    };

    static const float pattern1[8][2] = {
        { -0.100f,   0.000f },
        {  0.100f,   0.000f },
        {  0.000f,   0.500f },
        {  0.000f,  -0.500f },
        { -0.800f,   0.600f },
        {  0.800f,   0.600f },
        { -0.800f,  -0.600f },
        {  0.800f,  -0.600f }
    };

    const float (*pattern)[2] = (variant == 0) ? pattern0 : pattern1;

    float baseX = pattern[pelletIdx][0];
    float baseY = pattern[pelletIdx][1];
    
    // Apply rotation
    float cosTheta = cosf(rotationAngle);
    float sinTheta = sinf(rotationAngle);

    *outX = baseX * cosTheta - baseY * sinTheta;
    *outY = baseX * sinTheta + baseY * cosTheta;
}

/**
 * 125 Hz fork: a 24-pellet pattern (ClauDe2, 2026-11-04). Equal-area rings: one pellet in the middle, 7 at sqrt(4.5/24) of the cone's radius
 * (one straight down) and 16 at sqrt(16/24) (four on the axes), so every pellet covers the same share of the cone.
 */
void CalculatePelletPosition_24Pellets(int pelletIdx, float rotationAngle, float* outX, float* outY)
{
    static const float pattern[24][2] = {
        {  0.0000f,  0.0000f },
        {  0.0000f, -0.4330f },
        {  0.3385f, -0.2700f },
        {  0.4222f,  0.0964f },
        {  0.1879f,  0.3901f },
        { -0.1879f,  0.3901f },
        { -0.4222f,  0.0964f },
        { -0.3385f, -0.2700f },
        {  0.0000f, -0.8165f },
        {  0.3125f, -0.7543f },
        {  0.5774f, -0.5774f },
        {  0.7543f, -0.3125f },
        {  0.8165f,  0.0000f },
        {  0.7543f,  0.3125f },
        {  0.5774f,  0.5774f },
        {  0.3125f,  0.7543f },
        {  0.0000f,  0.8165f },
        { -0.3125f,  0.7543f },
        { -0.5774f,  0.5774f },
        { -0.7543f,  0.3125f },
        { -0.8165f,  0.0000f },
        { -0.7543f, -0.3125f },
        { -0.5774f, -0.5774f },
        { -0.3125f, -0.7543f }
    };

    float baseX = pattern[pelletIdx][0];
    float baseY = pattern[pelletIdx][1];

    float cosTheta = cosf(rotationAngle);
    float sinTheta = sinf(rotationAngle);

    *outX = baseX * cosTheta - baseY * sinTheta;
    *outY = baseX * sinTheta + baseY * cosTheta;
}


// ===========================================================================================
// Original game functions
// ===========================================================================================


float randomf()
{
	return (float)(int)rand() / (float)RAND_MAX;
}
inline void FastSinCos(const float value, float *pSin, float *pCos)
{
	*pSin = sin(value);
	*pCos = cos(value);
}
void gunrandom( float *x, float *y )
{
	float sinT, cosT, r, theta;

	theta = randomf() * 360;
	r = randomf();

	FastSinCos(theta * RADINDEG, &sinT, &cosT);

	*x = r * cosT;
	*y = r * sinT;
}


/*
===============
Bullet_Endpos
===============
*/
void Bullet_Endpos( float spread, vec3_t end, const weaponParms *wp, float maxRange, int shotIndex )
{
	float right;
	float up;

	float aimOffset = tan( spread * RADINDEG ) * maxRange;

	gunrandom(&right, &up);

	right *= aimOffset;
	up    *= aimOffset;

	VectorMA(wp->muzzleTrace, maxRange, wp->forward, end);

	VectorMA(end, right, wp->right, end);
	VectorMA(end, up, wp->up, end);
}


/**
 * 125 Hz fork: a bullet that hits the world makes an impact event (its sound and effect on every client), one per pellet: 182 for a small
 * bullet, 183 for a rifle bullet (Bullet_Fire_Extended: 182 + rifleBullet). 24 pellets made three times the impact sounds of the 8-pellet
 * shotgun at once, so a 24-pellet shot's world impacts are sent as event 181 instead, which every client already handles as the shotgun's
 * own impact (CG_BulletHitEvent): the effects of iw_impacts.csv's shotgun_normal / shotgun_reflect (the stock files: the same as
 * bullet_small's) and the sounds bulletspray_small_<surface> (the stock files: the same as bullet_small_<surface>), which a mod can make
 * quieter without touching any other gun (ClauDe2 does). All 24 impacts are drawn; the pellets, their traces and damage are unchanged.
 */
static bool bullet_sprayImpactEvent = false;

// 0050ff70    gentity_t* G_TempEntity(vec3_t origin, int event), called in Bullet_Fire_Extended for a world impact
gentity_t* G_TempEntity_BulletImpact(vec3_t origin, int event)
{
    if (bullet_sprayImpactEvent && (event == 182 || event == 183))
        event = 181;
    gentity_t* ent;
    ASM_CALL(RETURN(ent), ADDR(0x0050ff70, 0x0811efc4), 2, PUSH(origin), PUSH(event));
    return ent;
}

/**
 * Wrapper for game's Bullet_Fire_Extended function.
 */
void Bullet_Fire_Extended(gentity_t* source, gentity_t* attacker, vec3_t start, vec3_t end, float spread, 
    int recursion, weaponParms* wp, gentity_t* weaponEnt, int gameTime)
{
    ASM_CALL(RETURN_VOID, ADDR(0x005276f0, 0x0811fe90), 9,
        PUSH(source),
        PUSH(attacker),
        PUSH(start),
        PUSH(end),
        PUSH(spread),
        PUSH(recursion),
        PUSH(wp),
        PUSH(weaponEnt),
        PUSH(gameTime)
    );
}


/*
===============
G_BulletFireSpread
===============
*/
void G_BulletFireSpread(gentity_t* weaponEnt, gentity_t* attacker, weaponParms* wp, int gameTime, float spread)
{
    vec3_t start;
    int shotCount;

	VectorCopy(wp->muzzleTrace, start);

    shotCount = wp->weapDef->shotCount;
    float minDamageRange = wp->weapDef->minDamageRange;
    
    // CoD2x start
    // Check if custom pellet spread is enabled and weapon is a shotgun (8 pellets; 125 Hz fork: or 24, one pattern)
    if (g_shotgun_spread_fix->value.boolean && (shotCount == 8 || shotCount == 24))
    {
        // Compute aim offset radius once (converts angular spread to world distance)
        float aimOffset = tanf(spread * RADINDEG) * minDamageRange;

        // Generate random rotation angle once per shot group (0 to 2π); 125 Hz fork: none when g_shotgun_spread_rotation is off
        float rotationAngle = g_shotgun_spread_rotation->value.boolean ? (randomf() * 2.0f * M_PI) : 0.0f;

        // Randomly choose between 2 patterns
        int patternVariant = (randomf() < 0.5f) ? 0 : 1;

        // Fire each pellet with calculated position from chosen pattern
        for (int i = 0; i < shotCount; i++)
        {
            vec3_t end;
            float pelletX, pelletY;

            if (shotCount == 24)
                CalculatePelletPosition_24Pellets(i, rotationAngle, &pelletX, &pelletY);
            else
                CalculatePelletPosition_8Pellets(i, rotationAngle, patternVariant, &pelletX, &pelletY);

            // Transform 2D pattern coords to 3D world space using weapon orientation
            // Formula: end = muzzleTrace + forward*range + right*x*radius + up*y*radius
            VectorMA(wp->muzzleTrace, minDamageRange, wp->forward, end);
            VectorMA(end, pelletX * aimOffset, wp->right, end);
            VectorMA(end, pelletY * aimOffset, wp->up, end);

            // Fire bullet to calculated endpoint
            bullet_sprayImpactEvent = shotCount == 24;    // 125 Hz fork: the shotgun's own impact event, see G_TempEntity_BulletImpact
            Bullet_Fire_Extended(weaponEnt, attacker, start, end, 1.0f, 0, wp, weaponEnt, gameTime);
            bullet_sprayImpactEvent = false;
        }
    }
    // CoD2x end
    else
    {
        // Random spread using game's default gunrandom() function
        for (int i = 0; i < shotCount; i++)
        {
            vec3_t end;
            
            // Use original bullet spread calculation
            Bullet_Endpos(spread, end, wp, minDamageRange, i);
            
            // Fire bullet
            bullet_sprayImpactEvent = shotCount == 24;    // 125 Hz fork: the shotgun's own impact event, see G_TempEntity_BulletImpact
            Bullet_Fire_Extended(weaponEnt, attacker, start, end, 1.0f, 0, wp, weaponEnt, gameTime);
            bullet_sprayImpactEvent = false;
        }
    }
}


// 00527af0    weaponDef* G_BulletFireSpread(weaponParms* wp @ eax, gentity_t* weaponEnt, gentity_t* attacker, int gameTime, float spread)
#if COD2X_WIN32
void __cdecl G_BulletFireSpread_Win32(gentity_t* weaponEnt, gentity_t* attacker, int gameTime, float spread)
{
    weaponParms* wp;
    ASM(movr, wp, "eax");
    
    G_BulletFireSpread(weaponEnt, attacker, wp, gameTime, spread);
}
#endif





/**
 * Returns how fast the player walks, as a scale of the player's speed. Called by PM_WalkMove.
 * The original multiplies it by moveSpeedScale of ps->weapon. A grenade is thrown with the other hand while ps->weapon stays
 * the weapon the player held, so a throw ran as fast as that weapon: 1.1 with a pistol or the M1 Carbine, 0.9 with the BAR.
 */
// 00515990    float PM_CmdScale_Walk(pmove_t* pm @ eax, usercmd_t* cmd @ ebx)
float PM_CmdScale_Walk(pmove_t* pm, usercmd_t* cmd)
{
    float scale;

    // Call the original function
    ASM_CALL(RETURN_ST0_FLOAT(scale), ADDR(0x00515990, 0x080e1c58), WL(0, 2), WL(EAX, PUSH)(pm), WL(EBX, PUSH)(cmd));

    // CoD2x: 125 Hz fork: from pulling the pin to the end of the throw no weapon is in the hands,
    // so g_grenadeMoveSpeedScale replaces the scale of the weapon
    playerState_t* ps = pm->ps;
    float grenadeScale = g_grenadeMoveSpeedScale->value.decimal;
    if (grenadeScale > 0 && ps->weaponstate >= WEAPON_OFFHAND_PREPARE && ps->weaponstate <= WEAPON_OFFHAND_END)
    {
        float weaponScale = ps->weapon ? bg_weaponDefs[ps->weapon]->moveSpeedScale : 0.0f;
        if (weaponScale > 0)
            scale /= weaponScale;
        scale *= grenadeScale;
    }
    // CoD2x: End

    return scale;
}

#if COD2X_WIN32
float PM_CmdScale_Walk_Win32()
{
    pmove_t* pm;
    usercmd_t* cmd;
    ASM(movr, pm, "eax");
    ASM(movr, cmd, "ebx");

    return PM_CmdScale_Walk(pm, cmd);
}
#endif





/** Called once when hot-reloading is activated. */
void weapons_unload()
{

}


/**
 * Initialize weapons module on game start.
 */
void weapons_init()
{
	g_shotgun_spread_fix = Dvar_RegisterBool("g_shotgun_spread_fix", true, (enum dvarFlags_e)(DVAR_CHEAT | DVAR_CHANGEABLE_RESET));
	// 125 Hz fork: not cheat protected, so a mod can turn the rotation off for its own patterns
	g_shotgun_spread_rotation = Dvar_RegisterBool("g_shotgun_spread_rotation", true, (enum dvarFlags_e)(DVAR_CHANGEABLE_RESET));

	// 125 Hz fork: servers use pistol speed while a grenade is thrown. The value is sent to clients for their movement prediction,
	// clients keep 0 (original) until a server sends it and go back to 0 when they leave, see cgame_frame
	bool server = COD2X_LINUX || dedicated->value.integer > 0;
	dvarFlags_e noWriteForClientFlag = server ? DVAR_NOFLAG : DEBUG_RELEASE(DVAR_CHEAT, DVAR_NOWRITE);
	g_grenadeMoveSpeedScale = Dvar_RegisterFloat("g_grenadeMoveSpeedScale", server ? 1.1f : 0.0f, 0.0f, 2.0f, (enum dvarFlags_e)(noWriteForClientFlag | DVAR_SYSTEMINFO | DVAR_CHANGEABLE_RESET));
}


/**
 * Apply memory patches to hook weapon spread functions.
 */
void weapons_patch()
{
    // Hook G_BulletFireSpread to replace shotgun spread calculation
    patch_call(ADDR(0x00527baf, 0x081204d0), (unsigned int)WL(G_BulletFireSpread_Win32, G_BulletFireSpread));

    // 125 Hz fork: walk speed while throwing a grenade
    patch_call(ADDR(0x00516163, 0x080e268a), (unsigned int)WL(PM_CmdScale_Walk_Win32, PM_CmdScale_Walk)); // in PM_WalkMove

    // 125 Hz fork: the world impact events of a bullet, see G_TempEntity_BulletImpact. Only changed while the call still holds its original bytes
    std::vector<tickrate::Patch> plan;
#if COD2X_WIN32
    tickrate::redirect(plan, 0x005278c5, "bullet impact event", {0xe8,0xa6,0x86,0xfe,0xff}, (const void*)&G_TempEntity_BulletImpact);
#else
    tickrate::redirect(plan, 0x081200c8, "bullet impact event", {0xe8,0xf7,0xee,0xff,0xff}, (const void*)&G_TempEntity_BulletImpact);
#endif
    if (tickrate::verifyPatches(plan))
        tickrate::applyPatches(plan);
}
