//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
//=============================================================================//

#include "cbase.h"
#include "hud.h"
#include "hudelement.h"
#include "hud_macros.h"
#include "iclientmode.h"
#include "igameevents.h"
#include "cdll_int.h"

#include <vgui/ISurface.h>
#include <vgui/ILocalize.h>
#include <vgui_controls/AnimationController.h>
#include <vgui_controls/EditablePanel.h>
#include <vgui_controls/Label.h>
#include <vgui_controls/VectorImagePanel.h>
#include <vgui_controls/ImagePanel.h>

#include "c_cs_player.h"
#include "cs_client_gamestats.h"

using namespace vgui;

extern ConVar cl_hud_healthammo_style;
extern ConVar cl_hud_background_alpha;
extern ConVar cl_hud_color;
extern ConVar loadout_stattrak;
extern ConVar cl_draw_only_deathnotices;

#define MAX_KILL_ICONS 6

//-----------------------------------------------------------------------------
// Purpose: Displays current ammunition level
//-----------------------------------------------------------------------------
class CHudAmmo : public CHudElement, public EditablePanel
{
    DECLARE_CLASS_SIMPLE( CHudAmmo, EditablePanel );
 
public:
	CHudAmmo( const char *pElementName );
	virtual void Init( void );
	virtual void ApplySettings( KeyValues *inResourceData );
	virtual void Reset( void );
	virtual void OnThink();
	virtual void OnScreenSizeChanged( int iOldWide, int iOldTall );
	virtual bool ShouldDraw();

	// 事件回调函数
	virtual void FireGameEvent( IGameEvent *event );
	
private:
	CHandle<C_WeaponCSBase>	m_pActiveWeapon;

	Label				*m_pPrimaryAmmoLabel;
	Label				*m_pPrimaryReserveAmmoLabel;
	Label				*m_pStatTrakCounter;
	Label				*m_pKillCounter;
	ImagePanel			*m_pKillCounterImage;
	ImagePanel			*m_pKillIcons[MAX_KILL_ICONS];
	VectorImagePanel	*m_pBulletIcon;
	VectorImagePanel	*m_pExhaustibleWeaponIcon;
	VectorImagePanel	*m_pBurstIcon;

	CPanelAnimationVarAliasType( int, simple_wide, "simple_wide", "0", "proportional_width" );
	CPanelAnimationVarAliasType( int, simple_tall, "simple_tall", "0", "proportional_height" );

	bool	m_bUsesClips;
	bool	m_bIsExhaustible;
	int		m_iAmmoCount;
	bool	m_bBurstMode;
	
	int		m_iSimpleXPos;
	int		m_iSimpleYPos;
	int		m_iOriginalXPos;
	int		m_iOriginalYPos;
	int		m_iOriginalWide;
	int		m_iOriginalTall;

	// 用于跟踪当前生命周期的击杀类型
	int		m_iTrackedKills;
	bool	m_bKillIsHeadshot[MAX_KILL_ICONS];

	// 新增：从 .res 中读取的爆头图标路径
	char	m_szHeadshotIcon[MAX_PATH];
};

DECLARE_HUDELEMENT( CHudAmmo );

//-----------------------------------------------------------------------------
// Purpose: Constructor
//-----------------------------------------------------------------------------
CHudAmmo::CHudAmmo( const char *pElementName ): CHudElement( pElementName ), EditablePanel( NULL, "HudAmmo" )
{
	vgui::Panel *pParent = g_pClientMode->GetViewport();
	SetParent( pParent );

	SetHiddenBits( HIDEHUD_WEAPONSELECTION | HIDEHUD_NOT_OBSERVING_PLAYERS );

	m_iSimpleXPos = 0;
	m_iSimpleYPos = 0;
	m_iOriginalXPos = 0;
	m_iOriginalYPos = 0;
	m_iOriginalWide = 0;
	m_iOriginalTall = 0;

	m_iTrackedKills = 0;
	for ( int i = 0; i < MAX_KILL_ICONS; i++ )
		m_bKillIsHeadshot[i] = false;

	m_szHeadshotIcon[0] = '\0';

	m_pActiveWeapon = NULL;

	m_pPrimaryAmmoLabel = new Label( this, "PrimaryAmmoLabel", L"10" );
	m_pPrimaryReserveAmmoLabel = new Label( this, "PrimaryReserveAmmoLabel", L"/ 20" );
	m_pStatTrakCounter = new Label( this, "StatTrakCounter", L"0" );
	m_pKillCounter = new Label( this, "KillCounter", L"x0" );
	m_pKillCounterImage = new ImagePanel( this, "KillCounterImage" );

	for ( int i = 0; i < MAX_KILL_ICONS; i++ )
	{
		char szName[32];
		Q_snprintf( szName, sizeof( szName ), "KillIcon_%d", i );
		m_pKillIcons[i] = new ImagePanel( this, szName );
	}

	m_pBulletIcon = new VectorImagePanel( this, "BulletIcon" );
	m_pExhaustibleWeaponIcon = new VectorImagePanel( this, "ExhaustibleWeaponIcon" );
	m_pBurstIcon = new VectorImagePanel( this, "BurstIcon" );

	LoadControlSettings( "resource/hud/ammo.res" );
}

void CHudAmmo::OnScreenSizeChanged( int iOldWide, int iOldTall )
{
	// reload the .res file so items are rescaled
	LoadControlSettings( "resource/hud/ammo.res" );

	// force recalculation of some stuff
	m_iHUDColor = -1;
	m_flBackgroundAlpha = 0.0f;
	m_iStyle = -1;
	m_bUsesClips = false;
	m_bIsExhaustible = false;
	m_iAmmoCount = 0;
	m_bBurstMode = false;
}

void CHudAmmo::Init( void )
{
	m_bUsesClips		= false;
	m_bIsExhaustible	= false;
	m_iAmmoCount		= 0;
	m_bBurstMode		= false;

	m_iTrackedKills = 0;
	for ( int i = 0; i < MAX_KILL_ICONS; i++ )
		m_bKillIsHeadshot[i] = false;

	// 注册监听死亡事件
	ListenForGameEvent( "player_death" );
}

//-----------------------------------------------------------------------------
// Purpose: 监听事件，记录是否是爆头击杀
//-----------------------------------------------------------------------------
void CHudAmmo::FireGameEvent( IGameEvent *event )
{
	const char *name = event->GetName();
	if ( !Q_strcmp( name, "player_death" ) )
	{
		C_CSPlayer *pLocal = C_CSPlayer::GetLocalCSPlayer();
		if ( !pLocal ) return;

		int attackerID = event->GetInt( "attacker" );
		int victimID = event->GetInt( "userid" );

		// 确认是我们（本地玩家）击杀了别人（非自杀）
		if ( engine->GetPlayerForUserID( attackerID ) == pLocal->entindex() && 
			 attackerID != victimID )
		{
			bool bHeadshot = event->GetBool( "headshot" );

			if ( m_iTrackedKills < MAX_KILL_ICONS )
			{
				m_bKillIsHeadshot[m_iTrackedKills] = bHeadshot;
			}
			m_iTrackedKills++;
		}
	}
}

void CHudAmmo::ApplySettings( KeyValues *inResourceData )
{
	BaseClass::ApplySettings( inResourceData );

	// 从 ammo.res 中读取 headshot_icon 字段，未填写时默认使用 vgui/hud/kill_headshot_icon
	Q_strncpy( m_szHeadshotIcon, inResourceData->GetString( "headshot_icon", "vgui/hud/kill_headshot_icon" ), sizeof( m_szHeadshotIcon ) );

	GetBounds( m_iOriginalXPos, m_iOriginalYPos, m_iOriginalWide, m_iOriginalTall );

	int alignScreenWide, alignScreenTall;
	surface()->GetScreenSize( alignScreenWide, alignScreenTall );

	ComputePos( this, inResourceData->GetString( "simple_xpos", NULL ), m_iSimpleXPos, simple_wide, alignScreenWide, m_iBaseResolutionOverride[0], m_iBaseResolutionOverride[1], true, OP_SET );
	ComputePos( this, inResourceData->GetString( "simple_ypos", NULL ), m_iSimpleYPos, simple_tall, alignScreenTall, m_iBaseResolutionOverride[0], m_iBaseResolutionOverride[1], false, OP_SET );

	int killImgX, killImgY, killImgW, killImgH;
	m_pKillCounterImage->GetBounds( killImgX, killImgY, killImgW, killImgH );
	const char *pszImageName = m_pKillCounterImage->GetImageName();

	for ( int i = 0; i < MAX_KILL_ICONS; i++ )
	{
		m_pKillIcons[i]->SetBounds( killImgX, killImgY, killImgW, killImgH );
		m_pKillIcons[i]->SetImage( pszImageName );
		m_pKillIcons[i]->SetShouldScaleImage( m_pKillCounterImage->GetShouldScaleImage() );
		m_pKillIcons[i]->SetVisible( false );
	}
}

void CHudAmmo::Reset()
{
	g_pClientMode->GetViewportAnimationController()->StartAnimationSequence( gHUD.GetSequenceNameForHUDColor( "AmmoCounterReset", m_iHUDColor ) );
}

//-----------------------------------------------------------------------------
// Purpose: called every frame to get ammo info from the weapon
//-----------------------------------------------------------------------------
void CHudAmmo::OnThink()
{
	bool bWeaponChanged = false;
	if ( m_iStyle != cl_hud_healthammo_style.GetInt() )
	{
		m_iStyle = cl_hud_healthammo_style.GetInt();
		bWeaponChanged = true;

		switch ( m_iStyle )
		{
			case 0: // default
				SetBounds( m_iOriginalXPos, m_iOriginalYPos, m_iOriginalWide, m_iOriginalTall );
				break;

			case 1: // simple
				SetBounds( m_iSimpleXPos, m_iSimpleYPos, simple_wide, simple_tall );
				break;
		}
	}

	if ( m_flBackgroundAlpha != cl_hud_background_alpha.GetFloat() )
	{
		Color newColor = GetBgColor();
		newColor[3] = cl_hud_background_alpha.GetFloat() * 255;
		SetBgColor( newColor );
	}

	if ( m_iHUDColor != cl_hud_color.GetInt() )
	{
		m_iHUDColor = cl_hud_color.GetInt();
		Color clr = gHUD.GetHUDColor( m_iHUDColor );

		m_pPrimaryAmmoLabel->SetFgColor( clr );
		m_pPrimaryReserveAmmoLabel->SetFgColor( clr );
		m_pBulletIcon->SetFgColor( clr );
	}

	C_CSPlayer *pPlayer = GetHudPlayer();
	if ( !pPlayer )
	{
		SetPaintEnabled( false );
		SetPaintBackgroundEnabled( false );
		return;
	}

	C_WeaponCSBase *pWeapon = pPlayer->GetActiveCSWeapon();
	if ( !pWeapon )
	{
		m_pActiveWeapon = NULL;
		SetPaintEnabled( false );
		SetPaintBackgroundEnabled( false );
		return;
	}

	SetPaintEnabled( true );

	if ( pWeapon != m_pActiveWeapon )
	{
		m_pActiveWeapon = pWeapon;
		bWeaponChanged = true;
	}

	if ( bWeaponChanged )
	{
		m_bUsesClips = !(m_pActiveWeapon->GetWpnData().iFlags & ITEM_FLAG_EXHAUSTIBLE) && m_pActiveWeapon->UsesClipsForAmmo1();
		m_bIsExhaustible = (m_pActiveWeapon->GetWpnData().iFlags & ITEM_FLAG_EXHAUSTIBLE) && !m_pActiveWeapon->UsesClipsForAmmo1();

		SetPaintBackgroundEnabled( m_bUsesClips );

		m_pPrimaryAmmoLabel->SetVisible( m_bUsesClips );
		m_pPrimaryReserveAmmoLabel->SetVisible( m_bUsesClips );

		m_pBulletIcon->SetVisible( m_bUsesClips && (m_iStyle == 0) );

		m_pExhaustibleWeaponIcon->SetVisible( m_bIsExhaustible && (m_iStyle == 0) );
		m_pBurstIcon->SetVisible( m_pActiveWeapon->WeaponHasBurst() );
	}

	if ( m_bUsesClips )
	{
		if ( m_iAmmoCount < m_pActiveWeapon->Clip1() )
			g_pClientMode->GetViewportAnimationController()->StartAnimationSequence( gHUD.GetSequenceNameForHUDColor( "AmmoCounterReset", m_iHUDColor ) );

		m_iAmmoCount = m_pActiveWeapon->Clip1();

		if ( ((float) m_iAmmoCount) / ((float) pWeapon->GetMaxClip1()) <= 0.2f )
			g_pClientMode->GetViewportAnimationController()->StartAnimationSequence( "AmmoCounterLow" );

		wchar_t unicode[8];
		V_snwprintf( unicode, ARRAYSIZE( unicode ), L"%d", m_iAmmoCount );
		m_pPrimaryAmmoLabel->SetText( unicode );

		V_snwprintf( unicode, ARRAYSIZE( unicode ), L"/ %d", m_pActiveWeapon->GetReserveAmmoCount( AMMO_POSITION_PRIMARY ) );
		m_pPrimaryReserveAmmoLabel->SetText( unicode );

		m_pBulletIcon->SetRepeatsCount( Clamp( m_iAmmoCount, 0, 5 ) );

		if ( m_bBurstMode != m_pActiveWeapon->IsInBurstMode() )
		{
			m_bBurstMode = m_pActiveWeapon->IsInBurstMode();
			m_pBurstIcon->SetTexture( m_bBurstMode ? "materials/vgui/hud/svg/bullet_burst.svg" : "materials/vgui/hud/svg/bullet_burst_outline.svg" );
		}
	}

	if ( m_bIsExhaustible && (m_iStyle == 0) )
	{
		m_pExhaustibleWeaponIcon->SetRepeatsCount( pPlayer->GetAmmoCount( pWeapon->GetPrimaryAmmoType() ) );
		if ( bWeaponChanged )
			m_pExhaustibleWeaponIcon->SetTexture( UTIL_VarArgs( "materials/vgui/weapons/svg/%s.svg", pWeapon->GetClassname() + 7 ) );
	}

	wchar_t wszString[8];
	if ( pWeapon->HasStatTrak() )
	{
		int iStatTrakCounter = 0;
		int entindex = pPlayer->entindex();
		if ( pPlayer->IsControllingBot() )
			entindex = pPlayer->GetControlledBotIndex();

		if ( pWeapon->GetOriginalOwnerIndex() == entindex )
			iStatTrakCounter = g_CSClientGameStats.GetStatById( GetWeaponTableEntryFromWeaponId( pWeapon->GetCSWeaponID() ).killStatId ).iStatValue;
		else
			iStatTrakCounter = 0;

		if ( iStatTrakCounter > 0 )
		{
			V_snwprintf( wszString, sizeof( wszString ), L"%d", iStatTrakCounter );
			wchar_t wszLocalized[32];
			g_pVGuiLocalize->ConstructString( wszLocalized, sizeof( wszLocalized ), g_pVGuiLocalize->Find( "#Cstrike_WPNHUD_StatTrak" ), 1, wszString );
			m_pStatTrakCounter->SetText( wszLocalized );
			m_pStatTrakCounter->SetVisible( true );
		}
		else
		{
			m_pStatTrakCounter->SetVisible( false );
		}
	}
	else
	{
		m_pStatTrakCounter->SetVisible( false );
	}

	// === 击杀图标排版核心逻辑 ===
	int iNumKills = pPlayer->GetNumKillsThisSpawn();

	// 如果玩家刚复活（击杀数为0），重置历史记录
	if ( iNumKills == 0 )
	{
		m_iTrackedKills = 0;
		for ( int i = 0; i < MAX_KILL_ICONS; i++ )
			m_bKillIsHeadshot[i] = false;
	}

	if ( iNumKills > 0 )
	{
		if ( iNumKills <= MAX_KILL_ICONS )
		{
			m_pKillCounter->SetVisible( false );
			m_pKillCounterImage->SetVisible( false );

			int baseX, baseY, iconW, iconH;
			m_pKillCounterImage->GetBounds( baseX, baseY, iconW, iconH );
			int iconSpacing = iconW + scheme()->GetProportionalScaledValue( 2 );

			const char *pszNormalIcon = m_pKillCounterImage->GetImageName();
			// 读取在 ApplySettings 中解析到的 .res 爆头图标路径
			const char *pszHeadshotIcon = m_szHeadshotIcon;

			for ( int i = 0; i < MAX_KILL_ICONS; i++ )
			{
				if ( i < iNumKills )
				{
					int xPos = baseX - ( i * iconSpacing ); // 从右往左排列
					m_pKillIcons[i]->SetPos( xPos, baseY );
					
					// 判断这次击杀是否是爆头
					bool bIsHeadshot = ( i < m_iTrackedKills ) ? m_bKillIsHeadshot[i] : false;
					m_pKillIcons[i]->SetImage( bIsHeadshot ? pszHeadshotIcon : pszNormalIcon );

					m_pKillIcons[i]->SetVisible( true );
				}
				else
				{
					m_pKillIcons[i]->SetVisible( false );
				}
			}
		}
		else
		{
			// 击杀超过6人时，收起图标改回显示 xN 文本
			for ( int i = 0; i < MAX_KILL_ICONS; i++ )
			{
				m_pKillIcons[i]->SetVisible( false );
			}

			V_snwprintf( wszString, sizeof( wszString ), L"x%d", iNumKills );
			m_pKillCounter->SetText( wszString );
			m_pKillCounter->SetVisible( true );
			m_pKillCounterImage->SetImage( m_pKillCounterImage->GetImageName() );
			m_pKillCounterImage->SetVisible( true );
		}
	}
	else
	{
		m_pKillCounter->SetVisible( false );
		m_pKillCounterImage->SetVisible( false );
		for ( int i = 0; i < MAX_KILL_ICONS; i++ )
		{
			m_pKillIcons[i]->SetVisible( false );
		}
	}
}

bool CHudAmmo::ShouldDraw()
{
	if ( cl_draw_only_deathnotices.GetBool() )
		return false;

	return CHudElement::ShouldDraw();
}
