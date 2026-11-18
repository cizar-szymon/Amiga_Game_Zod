#include "gmm_main_menu.h"
#include "zod_version.h"

gmm_warning_flag GMMMainMenu::QuitWarningFlags()
{
	gmm_warning_flag flags;

	flags.text1 = "Are you sure you want";
	flags.text2 = "to quit the game?";
	flags.quit_game = true;

	return flags;
}

GMMMainMenu::GMMMainMenu() : ZGuiMainMenuBase()
{
	int i;

	menu_type = GMM_MAIN_MAIN;
	title = "Main Menu";
	w = 112;
	h = 118;

	//setup buttons
	for(i=0;i<MAX_GMMMM_BUTTONS;i++)
	{
		menu_button[i].SetType(MMGENERIC_BUTTON);
		menu_button[i].SetText(gmm_main_menu_button_string[i]);
		menu_button[i].SetCoords(GMM_SIDE_MARGIN, GMM_TITLE_MARGIN + i*(GMMWBUTTON_HEIGHT+1));
		menu_button[i].SetDimensions(w - (GMM_SIDE_MARGIN * 2), GMMWBUTTON_HEIGHT);
		AddWidget(&menu_button[i]);
	}

	//Warp: die Nummer der Ausgabe unter den Knoepfen -- damit ein
	//Fehlerbericht sagt, welcher Stand lief (port/zod_version.h).
	version_label.SetFont(SMALL_WHITE_FONT);
	version_label.SetText("Warp " ZOD_VERSION);
	version_label.SetCoords(GMM_SIDE_MARGIN, GMM_TITLE_MARGIN + i*(GMMWBUTTON_HEIGHT+1) + 1);
	version_label.SetDimensions(w - (GMM_SIDE_MARGIN * 2), MMLABEL_HEIGHT);
	version_label.SetJustification(MMLABEL_CENTER);
	AddWidget(&version_label);

	//cheap hack for h
	h = GMM_TITLE_MARGIN + i*(GMMWBUTTON_HEIGHT+1) + MMLABEL_HEIGHT + 2 + GMM_BOTTOM_MARGIN;

	//needed if w is ever changed / set
	UpdateDimensions();
}

void GMMMainMenu::HandleWidgetEvent(int event_type, ZGMMWidget *event_widget)
{
	int w_ref_id;

	if(!event_widget) return;

	w_ref_id = event_widget->GetRefID();

	switch(event_type)
	{
	case GMM_UNCLICK_EVENT:
		if(w_ref_id == menu_button[GMMMM_CHANGE_TEAMS_BUTTON].GetRefID())
		{
			gmm_flags.open_main_menu = true;
			gmm_flags.open_main_menu_type = GMM_CHANGE_TEAMS;
		}
		else if(w_ref_id == menu_button[GMMMM_MANAGE_BOTS_BUTTON].GetRefID())
		{
			gmm_flags.open_main_menu = true;
			gmm_flags.open_main_menu_type = GMM_MANAGE_BOTS;
		}
		else if(w_ref_id == menu_button[GMMMM_PLAYER_LIST_BUTTON].GetRefID())
		{
			gmm_flags.open_main_menu = true;
			gmm_flags.open_main_menu_type = GMM_PLAYER_LIST;
		}
		else if(w_ref_id == menu_button[GMMMM_SELECT_MAP_BUTTON].GetRefID())
		{
			gmm_flags.open_main_menu = true;
			gmm_flags.open_main_menu_type = GMM_SELECT_MAP;
		}
		else if(w_ref_id == menu_button[GMMMM_OPTIONS_BUTTON].GetRefID())
		{
			gmm_flags.open_main_menu = true;
			gmm_flags.open_main_menu_type = GMM_OPTIONS;
		}
		else if(w_ref_id == menu_button[GMMMM_QUIT_GAME_BUTTON].GetRefID())
		{
			gmm_flags.open_main_menu = true;
			gmm_flags.open_main_menu_type = GMM_WARNING;
			gmm_flags.warning_flags = QuitWarningFlags();
		}
		break;
	}
}
