#include "main.h" //tools here

// Dear ImGui - bloat-free immediate mode graphical user interface library for C++
// https://github.com/ocornut/imgui
#include "imgui/imgui.h"
#include "imgui/imgui_impl_win32.h"
#include "imgui/imgui_impl_opengl2.h"
// =============================================================================================== //

// =============================================================================================== //
// kutaQ3 hook - ImGui menu state
bool bImGuiReady = false;     // true once the ImGui backends are initialized
bool bInsideImgui = false;    // true while ImGui is rendering - hooked GL calls must pass straight through
bool bMenuShown = true;       // menu visibility (INSERT toggles) - shown on first injection
HWND g_GameHwnd = NULL;       // the game window the menu is attached to
WNDPROC oGameWndProc = NULL;  // the game's original window procedure

// feature toggles exposed in the "kutaQ3 hook" menu
bool bChamsEnabled = true;    // master toggle for the wallhack chams
int  iChamsStyle   = 0;       // 0 = solid, 1 = wireframe
bool bLogShaders   = true;    // log player shader names to log.txt while F10 is held
// =============================================================================================== //


void WINAPI newglBindTexture(GLenum target, GLuint texture)
{

	// ImGui's OpenGL2 backend binds its font atlas texture while it renders - those calls must go
	// straight to the original, otherwise the ESI shader sniffing below would dereference a garbage
	// pointer and crash the game.
	if (bInsideImgui)
	{
		CurrentTexture = texture;
		(*origglBindTexture)(target, texture);
		return;
	}

	//get shader
	//Shader = *((char **)0x016BB418); //mov     eax, dword_16BB418

	//get shader
	__asm mov Shader, esi
	if (Shader > 0x1000 && texture != NULL && Shader != NULL)
		shaderfound = true;
	else shaderfound = false;

	//model rec
	if (shaderfound)
	{
		//FREE FOR ALL PLAYER MODELS
	     if (
			 //mynx default player model
			 (strstr((char*)Shader, "models/players/mynx/mynx_h.tga")) ||
			 (strstr((char*)Shader, "models/players/mynx/mynx.tga")) ||
			 (strstr((char*)Shader, "models/players/mynx/mynx_shiny.tga")) ||

			 //major default player model
			 (strstr((char*)Shader, "models/players/major/major.tga")) ||
			 (strstr((char*)Shader, "models/players/major/major_h.tga")) ||

			 //major daemia player model
			 (strstr((char*)Shader, "models/players/major/daemia.tga")) ||
			 (strstr((char*)Shader, "models/players/major/daemia2_h.tga")) ||


			 //lucy default player model
			 (strstr((char*)Shader, "models/players/lucy/lucy_h.tga")) ||
			 (strstr((char*)Shader, "models/players/lucy/lucy.tga")) ||

			 
			 //lucy angel player model
			 (strstr((char*)Shader, "models/players/lucy/angel_h.tga")) ||
			 (strstr((char*)Shader, "models/players/lucy/angel.tga")) ||


			 //orbb default player model
			 (strstr((char*)Shader, "models/players/orbb/orbb.tga")) ||
			 //(strstr((char*)Shader, "models/players/orbb/orbb_light.tga")) ||
			 (strstr((char*)Shader, "models/players/orbb/orbb_h2.tga")) ||
			// (strstr((char*)Shader, "models/players/orbb/orbb_tail.tga")) ||
 

			 //ranger default player model
			 (strstr((char*)Shader, "models/players/ranger/ranger.tga")) ||
			 (strstr((char*)Shader, "models/players/ranger/ranger_h.tga")) ||


			 //ranger wrack player model
			 (strstr((char*)Shader, "models/players/ranger/wrack.tga")) ||
			 (strstr((char*)Shader, "models/players/ranger/wrack_h.tga")) ||

			 //razor ID player model
			 (strstr((char*)Shader, "models/players/razor/id.tga")) ||
			 (strstr((char*)Shader, "models/players/razor/h_id.tga")) ||

			 
			 
			 //razor default player model
			 (strstr((char*)Shader, "models/players/razor/razor.tga")) ||
			 (strstr((char*)Shader, "models/players/razor/h_razor.tga")) ||
			 //(strstr((char*)Shader, "models/players/razor/razor_gogs.tga")) ||
			 //(strstr((char*)Shader, "models/players/razor/razor_gogs_fx.tga")) ||
			 

			 //razor patriot player model
			 (strstr((char*)Shader, "models/players/razor/patriot.tga")) ||
			 (strstr((char*)Shader, "models/players/razor/h_patriot.tga")) ||
	  

			 //sarge default player model
				 (strstr((char*)Shader, "models/players/sarge/band.tga")) ||
				 //(strstr((char*)Shader, "models/players/sarge/cigar.tga")) ||
				 //(strstr((char*)Shader, "models/players/sarge/cigar.glow.tga")) ||

				 //sarge krusade player model
				 (strstr((char*)Shader, "models/players/sarge/krusade.tga")) ||
				 //(strstr((char*)Shader, "models/players/sarge/null.tga")) ||


				 //sarge roderic player model
				 (strstr((char*)Shader, "models/players/sarge/roderic.tga")) ||
				 //(strstr((char*)Shader, "models/players/sarge/null.tga")) ||

				 //slash default player model
				 (strstr((char*)Shader, "models/players/slash/slash_h.tga")) ||
				 (strstr((char*)Shader, "models/players/slash/slash.tga")) ||
				//(strstr((char*)Shader, "models/players/slash/slashskate.tga")) ||

				 //slash grrl player model
				 (strstr((char*)Shader, "models/players/slash/grrl_h.tga")) ||
				 (strstr((char*)Shader, "models/players/slash/grrl.tga")) ||
				 //(strstr((char*)Shader, "models/players/slash/slashskate.tga")) ||


				 //slash yuriko player model
				 (strstr((char*)Shader, "models/players/slash/yuriko_h.tga")) ||
				 (strstr((char*)Shader, "models/players/slash/yuriko.tga")) ||
				//(strstr((char*)Shader, "models/players/slash/yurikoskate.tga"))

				
				 //sorlag default player model
				 (strstr((char*)Shader, "models/players/sorlag/sorlag.tga")) ||
				 (strstr((char*)Shader, "models/players/sorlag/sorlag_t.tga")) ||
 

				 // tankjr default player model
				 (strstr((char*)Shader, "models/players/tankjr/tankjr.tga")) ||
	

				 // uriel default player model
				 (strstr((char*)Shader, "models/players/uriel/uriel_h.tga")) ||
				 (strstr((char*)Shader, "models/players/uriel/uriel.tga")) ||
				 (strstr((char*)Shader, "models/players/uriel/uriel_w.tga")) ||


				 // uriel zael player model
				 (strstr((char*)Shader, "models/players/uriel/zael_h.tga")) ||
				 (strstr((char*)Shader, "models/players/uriel/zael.tga")) ||
				// (strstr((char*)Shader, "models/players/uriel/null.tga")) ||

				 
				  // visor default player model
				  (strstr((char*)Shader, "models/players/visor/visor.tga")) ||

				  // visor gorre player model
				  (strstr((char*)Shader, "models/players/visor/gorre.tga")) ||


				 // xaero default player model
			      (strstr((char*)Shader, "models/players/xaero/xaero_h.tga")) ||
			      (strstr((char*)Shader, "models/players/xaero/xaero.tga")) ||
				  //(strstr((char*)Shader, "models/players/xaero/xaero_a.tga")) ||
		

				// marge default player model
				(strstr((char*)Shader, "models/players/marge/marge.tga")) ||

					
					 //klesk default player model
					 (strstr((char*)Shader, "models/players/klesk/klesk_h.tga")) ||
					 (strstr((char*)Shader, "models/players/klesk/klesk.tga")) ||
					 (strstr((char*)Shader, "models/players/klesk/klesk_g.tga")) ||

					 //klesk flisk player model
					 (strstr((char*)Shader, "models/players/klesk/flisk_h.tga")) ||
					 (strstr((char*)Shader, "models/players/klesk/flisk.tga")) ||
					 		

					 //keel default player model
					 (strstr((char*)Shader, "models/players/Keel/keel_h.tga")) ||
					 (strstr((char*)Shader, "models/players/Keel/keel.tga")) ||


					 //hunter default player model
					 (strstr((char*)Shader, "models/players/hunter/hunter_h.tga")) ||
					 (strstr((char*)Shader, "models/players/hunter/hunter2.tga")) ||
					 //(strstr((char*)Shader, "models/players/hunter/hunter_f.tga")) ||

					 //hunter harpy player model
					 (strstr((char*)Shader, "models/players/hunter/hunter_h.tga")) ||
					 (strstr((char*)Shader, "models/players/hunter/hunter2.tga")) ||

					 //grunt stripe player model
					 (strstr((char*)Shader, "models/players/grunt/stripe_h.tga")) ||
					 (strstr((char*)Shader, "models/players/grunt/stripe.tga")) ||


					 //grunt default player model
					 (strstr((char*)Shader, "models/players/grunt/grunt_h.tga")) ||
					 (strstr((char*)Shader, "models/players/grunt/grunt.tga")) ||


					 //doom phobos player model
					 (strstr((char*)Shader, "models/players/doom/phobos.tga")) ||
					 (strstr((char*)Shader, "models/players/doom/phobos_fx.tga")) ||
					 (strstr((char*)Shader, "models/players/doom/phobos_f.tga")) ||


					 //doom default player model
					 (strstr((char*)Shader, "models/players/doom/doom.tga")) ||
					 (strstr((char*)Shader, "models/players/doom/doom_f.tga")) ||
					 (strstr((char*)Shader, "models/players/doom/doom_fx.tga")) ||	

					//crash default player model
					(strstr((char*)Shader, "models/players/crash/crash.tga")) ||
					(strstr((char*)Shader, "models/players/crash/crash_t.tga")) ||

					//bones bones player model
					(strstr((char*)Shader, "models/players/bones/stiff.tga")) ||

					//bones default player model
				    (strstr((char*)Shader, "models/players/bones/xray.tga")) ||

					//bitterman default player model
					(strstr((char*)Shader, "models/players/bitterman/h_bitterman.tga")) ||
					(strstr((char*)Shader, "models/players/bitterman/bitterman.tga")) ||

					//biker stroggo player model
					(strstr((char*)Shader, "models/players/biker/stroggo_h.tga")) ||
					(strstr((char*)Shader, "models/players/biker/stroggo.tga")) ||

					//biker slammer player model
					(strstr((char*)Shader, "models/players/biker/slammer_h.tga")) ||
					(strstr((char*)Shader, "models/players/biker/slammer.tga")) ||


					//biker hossman player model
					(strstr((char*)Shader, "models/players/biker/hossman_h.tga")) ||
				    (strstr((char*)Shader, "models/players/biker/hossman.tga")) ||

					//biker default player model
					(strstr((char*)Shader, "models/players/biker/biker_h.tga")) ||
					(strstr((char*)Shader, "models/players/biker/biker.tga")) ||

					//biker cadaver player model
					(strstr((char*)Shader, "models/players/biker/cadaver_h.tga")) ||
					(strstr((char*)Shader, "models/players/biker/cadaver.tga")) ||


					//anarki default player model
					(strstr((char*)Shader, "models/players/anarki/anarki.tga")) ||
					(strstr((char*)Shader, "models/players/anarki/anarki_b.tga")) ||
					(strstr((char*)Shader, "models/players/anarki/anarki_g.tga")) ||
					(strstr((char*)Shader, "models/players/anarki/anarki_g_fx.tga")) ||
					(strstr((char*)Shader, "models/players/anarki/anarki_h.tga")) ||

					//xian default player model
					(strstr((char*)Shader, "models/players/xian/xianleg.TGA")) ||
					(strstr((char*)Shader, "models/players/xian/xianarm2.TGA")) ||
					(strstr((char*)Shader, "models/players/xian/xiantors.TGA")) ||
					(strstr((char*)Shader, "models/players/medium/xian/xianfacf.tga")) ||
					(strstr((char*)Shader, "models/players/medium/xian/xianfacf.tga")) ||

					//tim default player model
					(strstr((char*)Shader, "models/players/tim/timleg.tga")) ||
					(strstr((char*)Shader, "models/players/tim/timarm.TGA")) ||
					(strstr((char*)Shader, "models/players/tim/timtors.TGA")) ||
					(strstr((char*)Shader, "models/players/tim/timface.TGA")) ||

					//paulj default player model
					(strstr((char*)Shader, "models/players/paulj/pjleg.TGA")) ||
					(strstr((char*)Shader, "models/players/paulj/pjarm.TGA")) ||
				    (strstr((char*)Shader, "models/players/paulj/pjtorso.TGA")) ||
				    (strstr((char*)Shader, "models/players/paulj/pjface.TGA")) ||

					//cash default player model
				    (strstr((char*)Shader, "models/players/cash/cashleg.TGA")) ||
				    (strstr((char*)Shader, "models/players/cash/cashface.TGA")) ||
				    (strstr((char*)Shader, "models/players/cash/cashtors.TGA")) ||
				    (strstr((char*)Shader, "models/players/cash/casharm.TGA")) ||

				    //carmack default player model
				    (strstr((char*)Shader, "models/players/carmack/john05.TGA")) ||
				    (strstr((char*)Shader, "models/players/carmack/john04.TGA")) ||
				    (strstr((char*)Shader, "models/players/carmack/john03.TGA")) ||
					(strstr((char*)Shader, "models/players/carmack/john07.TGA")) ||
				    (strstr((char*)Shader, "models/players/carmack/john01.TGA")) ||
					(strstr((char*)Shader, "models/players/carmack/john02.TGA")) ||
				    (strstr((char*)Shader, "models/players/carmack/john06.TGA")) ||

					//brandon default player model
					(strstr((char*)Shader, "models/players/brandon/branleg.tga")) ||
					(strstr((char*)Shader, "models/players/brandon/branarm.TGA")) ||
					(strstr((char*)Shader, "models/players/brandon/brantors.tga")) ||
					(strstr((char*)Shader, "models/players/light/brandon/branhat.tga")) ||
				    (strstr((char*)Shader, "models/players/brandon/branface.tga")) ||


					
					//CPMA doom player model
					(strstr((char*)Shader, "models/players/doom/color.tga")) ||

					//CPMA visor player model
					(strstr((char*)Shader, "models/players/visor/color.tga")) ||

					//CPMA ranger player model
					(strstr((char*)Shader, "models/players/ranger/color.tga")) ||
					(strstr((char*)Shader, "models/players/ranger/color_h.tga")) ||

					//CPMA crash player model
					(strstr((char*)Shader, "models/players/crash/color.tga")) ||
					(strstr((char*)Shader, "models/players/crash/color_t.tga")) ||

					//CPMA mynx player model
					(strstr((char*)Shader, "models/players/mynx/color.tga")) ||
					(strstr((char*)Shader, "models/players/mynx/color_x.tga")) ||
					(strstr((char*)Shader, "models/players/mynx/color_h.tga")) ||

					//CPMA tankjr player model
					(strstr((char*)Shader, "models/players/tankjr/color.tga")) ||

					//CPMA anarki player model
					(strstr((char*)Shader, "models/players/anarki/color.tga")) ||
					(strstr((char*)Shader, "models/players/anarki/color_h.tga")) ||
					(strstr((char*)Shader, "models/players/anarki/color_x.tga")) ||

					//CPMA biker player model
					(strstr((char*)Shader, "models/players/biker/color.tga")) ||
					(strstr((char*)Shader, "models/players/biker/color_h.tga")) ||

					//CPMA bitterman player model
					(strstr((char*)Shader, "models/players/bitterman/color.tga")) ||
					(strstr((char*)Shader, "models/players/bitterman/color_h.tga")) ||

					//CPMA bones player model
					(strstr((char*)Shader, "models/players/bones/color.tga")) ||

					//CPMA grunt player model
					(strstr((char*)Shader, "models/players/grunt/color.tga")) ||
					(strstr((char*)Shader, "models/players/grunt/color_h.tga")) ||

					//CPMA hunter player model
					(strstr((char*)Shader, "models/players/hunter/color.tga")) ||
					(strstr((char*)Shader, "models/players/hunter/color_h.tga")) ||
					//(strstr((char*)Shader, "models/players/hunter/red_f.tga")) ||

					//CPMA keel player model
					(strstr((char*)Shader, "models/players/keel/color.tga")) ||
					(strstr((char*)Shader, "models/players/keel/color_h.tga")) ||
					
					//CPMA klesk player model
					(strstr((char*)Shader, "models/players/klesk/color.tga")) ||
					(strstr((char*)Shader, "models/players/klesk/color_h.tga")) ||

					//CPMA lucy player model
					(strstr((char*)Shader, "models/players/lucy/color.tga")) ||
					(strstr((char*)Shader, "models/players/lucy/color_h.tga")) ||

					//CPMA major player model
					(strstr((char*)Shader, "models/players/major/color.tga")) ||
					(strstr((char*)Shader, "models/players/major/color_h.tga")) ||

					//CPMA orbb player model
					(strstr((char*)Shader, "models/players/orbb/color.tga")) ||
					(strstr((char*)Shader, "models/players/orbb/color_h.tga")) ||
					//(strstr((char*)Shader, "models/players/orbb/orbb_light.tga")) ||
					//(strstr((char*)Shader, "models/players/orbb/orbb_tail.tga")) ||

					//CPMA razor player model
					(strstr((char*)Shader, "models/players/razor/color.tga")) ||
					(strstr((char*)Shader, "models/players/razor/color_h.tga")) ||
					//(strstr((char*)Shader, "models/players/razor/razor_gogs.tga")) ||
					//(strstr((char*)Shader, "models/players/razor/razor_gogs_fx.tga")) ||

					//CPMA sarge player model
					(strstr((char*)Shader, "models/players/sarge/color.tga")) ||
					//(strstr((char*)Shader, "models/players/sarge/cigar.tga")) ||
					//(strstr((char*)Shader, "models/players/sarge/cigar.glow.tga")) ||  

					//CPMA slash player model
					(strstr((char*)Shader, "models/players/slash/color.tga")) ||
					(strstr((char*)Shader, "models/players/slash/color_h.tga")) ||
					//(strstr((char*)Shader, "models/players/slash/color_skates.tga")) ||  

					//CPMA sorlag player model
					(strstr((char*)Shader, "models/players/sorlag/color.tga")) ||
					(strstr((char*)Shader, "models/players/sorlag/color_t.tga")) ||

					//CPMA uriel player model
					(strstr((char*)Shader, "models/players/uriel/color.tga")) ||
					(strstr((char*)Shader, "models/players/uriel/color_h.tga")) ||
					//(strstr((char*)Shader, "models/players/uriel/null.tga")) ||

					//CPMA xaero player model
					(strstr((char*)Shader, "models/players/xaero/color.tga")) ||
					(strstr((char*)Shader, "models/players/xaero/color_h.tga")) 
					//(strstr((char*)Shader, "models/players/xaero/xaero_a.tga")) ||
				)


				free_for_all_player_models = true;
		else free_for_all_player_models = false;
		
		
		//BLUE TEAM PLAYER MODELS
		if (
			//mynx blue player model
			(strstr((char*)Shader, "models/players/mynx/blue_s.tga")) ||
			(strstr((char*)Shader, "models/players/mynx/mynx.tga")) ||
			(strstr((char*)Shader, "models/players/mynx/red_h.tga")) ||

			//major blue player model
			(strstr((char*)Shader, "models/players/major/blue.tga")) ||
			(strstr((char*)Shader, "models/players/major/blue_h.tga")) ||

			//lucy blue player model
			(strstr((char*)Shader, "models/players/lucy/blue.tga")) ||
			(strstr((char*)Shader, "models/players/lucy/blue_h.tga")) ||

			//orbb blue player model
			(strstr((char*)Shader, "models/players/orbb/blue.tga")) ||
			(strstr((char*)Shader, "models/players/orbb/orbb_light_blue.tga")) ||
			(strstr((char*)Shader, "models/players/orbb/blue_h.tga")) ||
			(strstr((char*)Shader, "models/players/orbb/orbb_tail_blue.tga")) ||

			//ranger blue player model
			(strstr((char*)Shader, "models/players/ranger/blue.tga")) ||
			(strstr((char*)Shader, "models/players/ranger/red_h.tga")) ||

			//razor blue player model
			(strstr((char*)Shader, "models/players/razor/blue.tga")) ||
			(strstr((char*)Shader, "models/players/razor/h_blue.tga")) ||
			//(strstr((char*)Shader, "models/players/razor/razor_gogs.tga")) ||
			//(strstr((char*)Shader, "models/players/razor/razor_gogs_fx.tga")) ||

			//sarge blue player model
			(strstr((char*)Shader, "models/players/sarge/blue.tga")) ||
			//(strstr((char*)Shader, "models/players/sarge/cigar.tga")) ||
			//(strstr((char*)Shader, "models/players/sarge/cigar.glow.tga")) ||

			//slash blue player model
			(strstr((char*)Shader, "models/players/slash/blue_h.tga")) ||
			(strstr((char*)Shader, "models/players/slash/blue.tga")) ||
			//(strstr((char*)Shader, "models/players/slash/slashskate.tga")) ||

			//sorlag blue player model
			(strstr((char*)Shader, "models/players/sorlag/blue.tga")) ||
			(strstr((char*)Shader, "models/players/sorlag/blue_t.tga")) ||

			// tankjr blue player model
			(strstr((char*)Shader, "models/players/tankjr/blue.tga")) ||

			// uriel blue player model
			(strstr((char*)Shader, "models/players/uriel/red_h.tga")) ||
			(strstr((char*)Shader, "models/players/uriel/blue.tga")) ||
			(strstr((char*)Shader, "models/players/uriel/blue_w.tga")) ||

			// visor blue player model
			(strstr((char*)Shader, "models/players/visor/blue.tga")) ||

			// xaero blue player model
			(strstr((char*)Shader, "models/players/xaero/blue_h.tga")) ||
			(strstr((char*)Shader, "models/players/xaero/blue.tga")) ||
			//(strstr((char*)Shader, "models/players/xaero/xaero_a.tga")) ||

			// marge blue player model
			(strstr((char*)Shader, "models/players/marge/marge_blue.tga")) ||

			//klesk blue player model
			(strstr((char*)Shader, "models/players/klesk/blue_h.tga")) ||
			(strstr((char*)Shader, "models/players/klesk/blue.tga")) ||

			//keel blue player model
			(strstr((char*)Shader, "models/players/Keel/blue_h.tga")) ||
			(strstr((char*)Shader, "models/players/Keel/blue.tga")) ||

			//hunter blue player model
			(strstr((char*)Shader, "models/players/hunter/blue_h.tga")) ||
			(strstr((char*)Shader, "models/players/hunter/blue.tga")) ||
			//(strstr((char*)Shader, "models/players/hunter/red_f.tga")) ||

			//grunt blue player model
			(strstr((char*)Shader, "models/players/grunt/blue_h.tga")) ||
			(strstr((char*)Shader, "models/players/grunt/blue.tga")) ||

			//doom blue player model
			(strstr((char*)Shader, "models/players/doom/blue.tga")) ||
			(strstr((char*)Shader, "models/players/doom/doom_f.tga")) ||
			(strstr((char*)Shader, "models/players/doom/doom_fx.tga")) ||

			//crash blue player model
			(strstr((char*)Shader, "models/players/crash/blue.tga")) ||
			(strstr((char*)Shader, "models/players/crash/blue_t.tga")) ||

			//bones blue player model
			(strstr((char*)Shader, "models/players/bones/blue.tga")) ||

			//bitterman blue player model
			(strstr((char*)Shader, "models/players/bitterman/h_blue.tga")) ||
			(strstr((char*)Shader, "models/players/bitterman/blue.tga")) ||

			//biker blue player model
			(strstr((char*)Shader, "models/players/biker/blue_h.tga")) ||
			(strstr((char*)Shader, "models/players/biker/blue.tga")) ||

			//anarki blue player model
			(strstr((char*)Shader, "models/players/anarki/red_h.tga")) ||
			(strstr((char*)Shader, "models/players/anarki/blue.tga")) ||
			(strstr((char*)Shader, "models/players/anarki/blue_g.tga")) ||
			(strstr((char*)Shader, "models/players/anarki/anarki_b.tga")) 
			)
	
		blue_team_player_models = true;
		else blue_team_player_models = false;
	

	//RED TEAM PLAYER MODELS
	if (
		//mynx red player model
		(strstr((char*)Shader, "models/players/mynx/red_s.tga")) ||
		(strstr((char*)Shader, "models/players/mynx/mynx.tga")) ||
		(strstr((char*)Shader, "models/players/mynx/red_h.tga")) ||

		//major red player model
		(strstr((char*)Shader, "models/players/major/red.tga")) ||
		(strstr((char*)Shader, "models/players/major/red_h.tga")) ||

		//lucy red player model
		(strstr((char*)Shader, "models/players/lucy/red_h.tga")) ||
		(strstr((char*)Shader, "models/players/lucy/red.tga")) ||

		//orbb red player model
		(strstr((char*)Shader, "models/players/orbb/red_h.tga")) ||
		(strstr((char*)Shader, "models/players/orbb/red.tga")) ||
		//(strstr((char*)Shader, "models/players/orbb/orbb_light.tga")) ||
		//(strstr((char*)Shader, "models/players/orbb/orbb_tail.tga")) ||

		//ranger red player model
		(strstr((char*)Shader, "models/players/ranger/red.tga")) ||
		(strstr((char*)Shader, "models/players/ranger/red_h.tga")) ||

		//razor red player model
		(strstr((char*)Shader, "models/players/razor/red.tga")) ||
		(strstr((char*)Shader, "models/players/razor/h_red.tga")) ||
		//(strstr((char*)Shader, "models/players/razor/razor_gogs.tga")) ||
		//(strstr((char*)Shader, "models/players/razor/razor_gogs_fx.tga")) 

		//sarge red player model
		(strstr((char*)Shader, "models/players/sarge/red.tga")) ||
		//(strstr((char*)Shader, "models/players/sarge/cigar.tga"))
		//(strstr((char*)Shader, "models/players/sarge/cigar.glow.tga"))

		//slash red player model
		(strstr((char*)Shader, "models/players/slash/red_h.tga")) ||
		(strstr((char*)Shader, "models/players/slash/red.tga")) ||
		//(strstr((char*)Shader, "models/players/slash/slashskate.tga")) ||

		//sorlag red player model
		(strstr((char*)Shader, "models/players/sorlag/red.tga")) ||
		(strstr((char*)Shader, "models/players/sorlag/red_t.tga")) ||

		// tankjr red player model
		(strstr((char*)Shader, "models/players/tankjr/red.tga")) ||

		// uriel red player model
		(strstr((char*)Shader, "models/players/uriel/red_h.tga")) ||
		(strstr((char*)Shader, "models/players/uriel/red.tga")) ||
		(strstr((char*)Shader, "models/players/uriel/red_w.tga")) ||

		// visor red player model
		(strstr((char*)Shader, "models/players/visor/red.tga")) ||

		// xaero red player model
		(strstr((char*)Shader, "models/players/xaero/red_h.tga")) ||
		(strstr((char*)Shader, "models/players/xaero/red.tga")) ||
		//(strstr((char*)Shader, "models/players/xaero/xaero_a.tga")) ||

		// marge red player model
		(strstr((char*)Shader, "models/players/marge/marge_red.tga")) ||

		//klesk red player model
		(strstr((char*)Shader, "models/players/klesk/red_h.tga")) ||
		(strstr((char*)Shader, "models/players/klesk/red.tga")) ||

		//keel red player model
		(strstr((char*)Shader, "models/players/Keel/red_h.tga")) ||
		(strstr((char*)Shader, "models/players/Keel/red.tga")) ||

		//hunter red player model
		(strstr((char*)Shader, "models/players/hunter/red_h.tga")) ||
		(strstr((char*)Shader, "models/players/hunter/red.tga")) ||
		// (strstr((char*)Shader, "models/players/hunter/red_f.tga")) ||

		//grunt red player model
		(strstr((char*)Shader, "models/players/grunt/red_h.tga")) ||
		(strstr((char*)Shader, "models/players/grunt/red.tga")) ||

		//doom red player model
		(strstr((char*)Shader, "models/players/doom/red.tga")) ||
		(strstr((char*)Shader, "models/players/doom/doom_f.tga")) ||
		(strstr((char*)Shader, "models/players/doom/doom_fx.tga")) ||

		//crash red player model
		(strstr((char*)Shader, "models/players/crash/red.tga")) ||
		(strstr((char*)Shader, "models/players/crash/red_t.tga")) ||

		//bones red player model
		(strstr((char*)Shader, "models/players/bones/red.tga")) ||

		//bitterman red player model
		(strstr((char*)Shader, "models/players/bitterman/h_red.tga")) ||
		(strstr((char*)Shader, "models/players/bitterman/red.tga")) ||

		//biker red player model
		(strstr((char*)Shader, "models/players/biker/red_h.tga")) ||
		(strstr((char*)Shader, "models/players/biker/red.tga")) ||

			//anarki red player model
			(strstr((char*)Shader, "models/players/anarki/red_h.tga")) ||
			(strstr((char*)Shader, "models/players/anarki/red.tga")) ||
			(strstr((char*)Shader, "models/players/anarki/red_g.tga")) ||
			(strstr((char*)Shader, "models/players/anarki/anarki_b.tga")) 
		)

		red_team_player_models = true;
	else red_team_player_models = false;
	}


	/*/
		//log shader names
		if(GetAsyncKeyState(VK_F10) < 0)
		if(texture != NULL && Shader != NULL)
	    if(allaxis || allallies)
		Log("Shader == %s\r", Shader);
		*/ 
	
	//the way I logged players for Quake 3
		if (bLogShaders)
		if (GetAsyncKeyState(VK_F10) < 0)
			if (texture != NULL && Shader != NULL)
				if (strstr((char*)Shader, "models/players"))
					Log("Shader == %s\r", Shader);

	// call original

		CurrentTexture = texture;
	

	(*origglBindTexture)(target, texture);
}




void DisableDepthTest()
{
	_asm
	{
		push GL_POINTS
		add dword ptr ss : [esp], GL_DEPTH_TEST - GL_POINTS
		call dword ptr[glDisable]
	}
}

void EnableDepthTest()
{
	_asm
	{
		push GL_POINTS
		add dword ptr ss : [esp], GL_DEPTH_TEST - GL_POINTS
		call dword ptr[glEnable]
	}
}


void TransColorFunc(int r, int g, int b, int a);
void TransChams(int r, int g, int b, int a, int r2, int g2, int b2, int a2, GLenum mode, GLsizei count, GLenum type, const GLvoid *indices);

////http://aimbots.net/threads/2437-etx-plugins
void TransColorFunc(int r, int g, int b, int a)
{
	glDisableClientState(GL_COLOR_ARRAY);
	glEnable(GL_COLOR_MATERIAL);
	glColor4ub(r, g, b, a);
}

void TransChams(int r, int g, int b, int a, int r2, int g2, int b2, int a2, GLenum mode, GLsizei count, GLenum type, const GLvoid *indices)
{
	glPushMatrix();
	glDisable(GL_TEXTURE_2D);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDisable(GL_DEPTH_TEST);
	TransColorFunc(r, g, b, a);
	origglDrawElements(mode, count, type, indices);
	TransColorFunc(r2, g2, b2, a2);
	glEnable(GL_DEPTH_TEST);
	glPopMatrix();
}

// =============================================================================================== //
// chams styles selectable from the "kutaQ3 hook" menu

// solid chams: flat colour behind walls + flat colour in front of walls
void DrawChamsSolid(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices)
{
	if (free_for_all_player_models)
	{
		TransChams(255, 0, 255, 120, 0, 255, 0, 255, mode, count, type, indices); //Behind wall PINK / Infront wall GREEN - in this order (RGBA)
	}
	else if (blue_team_player_models)
	{
		TransChams(255, 255, 255, 120, 0, 255, 255, 255, mode, count, type, indices); //Behind wall WHITE / Infront wall BLUE - in this order (RGBA)
	}
	else if (red_team_player_models)
	{
		TransChams(255, 80, 0, 255, 255, 0, 0, 255, mode, count, type, indices); //Behind wall YELLOW / Infront wall RED - in this order (RGBA)
	}
}

// wireframe chams: wireframe outline behind walls + solid colour in front of walls
void DrawChamsWireframe(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices)
{
	glPushMatrix();
	DisableDepthTest(); //glDisable(GL_DEPTH_TEST);
	glDisable(GL_TEXTURE_2D);
	ColorFunc(0, 0, 0, 255); //black
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glEnable(GL_POLYGON_OFFSET_LINE);
	glEnable(GL_LINE_LOOP);
	glLineWidth(2.5);
	glPolygonMode(GL_FRONT, GL_LINE);
	glEnable(GL_CULL_FACE);
	glCullFace(GL_BACK);
	if (free_for_all_player_models)
		glColor4ub(255, 0, 255, 255); //behind walls (pink colour)
	else if (red_team_player_models)
		glColor4ub(255, 80, 0, 255); //behind walls (yellow colour)
	else if (blue_team_player_models)
		glColor4ub(255, 255, 255, 255); //behind walls (white colour)
	origglDrawElements(mode, count, type, indices);

	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glEnable(GL_DEPTH_TEST);
	glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	glScalef(1, 1, 1);
	if (free_for_all_player_models)
		glColor4ub(38, 255, 38, 255); //infront of walls (green colour)
	else if (red_team_player_models)
		glColor4ub(255, 0, 0, 255); //infront of walls (red colour)
	else if (blue_team_player_models)
		glColor4ub(0, 255, 255, 255); //infront of walls (blue colour)
	origglDrawElements(mode, count, type, indices);

	glEnable(GL_TEXTURE_2D);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glScalef(0.99, 1.01, 1.0);
	EnableDepthTest(); //glEnable(GL_DEPTH_TEST);
	origglDrawElements(mode, count, type, indices);
	ColorFunc(20, 20, 20, 120); //black
	glPopMatrix();
}
// =============================================================================================== //




void WINAPI newglDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices)
{
	// ImGui's OpenGL2 backend also draws indexed geometry - pass its draw calls through untouched,
	// otherwise the menu would get tinted/transparent because of the chams code below.
	if (bInsideImgui)
	{
		// call original
		(*origglDrawElements)(mode, count, type, indices);
		return;
	}

	//chams solid - working
	if (bChamsEnabled && (free_for_all_player_models || red_team_player_models || blue_team_player_models))
	{
		if (iChamsStyle == 1)
		{
			DrawChamsWireframe(mode, count, type, indices); //wireframe outline behind walls / solid colour infront of walls
		}
		else
		{
			DrawChamsSolid(mode, count, type, indices); //solid colour behind walls / solid colour infront of walls
		}
	}
	else
	{
		glEnable(GL_TEXTURE_2D);
	}


	/*
	//chams normal - working
	if (free_for_all_player_models || red_team_player_models || blue_team_player_models)
	{
		glPushMatrix();
		DisableDepthTest();//glDisable(GL_DEPTH_TEST);
		glDisable(GL_TEXTURE_2D);
		glEnable(GL_COLOR_MATERIAL);
		glDisableClientState(GL_COLOR_ARRAY);
		if (red_team_player_models)
			glColor4ub(255, 80, 0, 255); //behind walls (yellow colour)
		else if (free_for_all_player_models)
			glColor4ub(255, 0, 255, 255); //behind walls (pink colour)
		else if (blue_team_player_models)
			glColor4ub(255, 255, 255, 255); //behind walls (white colour)
		origglDrawElements(mode, count, type, indices);
		glEnable(GL_COLOR_MATERIAL);
		glDisableClientState(GL_COLOR_ARRAY);
		if (red_team_player_models)
			glColor4ub(255, 0, 0, 255); //infront of walls (red colour)
		else if (free_for_all_player_models)
			glColor4ub(38, 255, 38, 255); //infront of  walls (green colour)
		else if (blue_team_player_models)
			glColor4ub(0, 255, 255, 255); //infront of walls (blue colour)
		glEnable(GL_TEXTURE_2D);
		EnableDepthTest();//glEnable(GL_DEPTH_TEST);
		glPopMatrix();
	}
	*/
	
	
	

	/*
	//chams wired -testing at the moment
	if (free_for_all_player_models)
	{

		glPushMatrix();
		glDisable(GL_TEXTURE_2D);
		glDisable(GL_DEPTH_TEST);
		ColorFunc(0, 0, 0, 255);//black
		glDisableClientState(GL_TEXTURE_COORD_ARRAY);
		glEnable(GL_POLYGON_OFFSET_LINE);
		glEnable(GL_LINE_LOOP);
		glLineWidth(2.5);
		glPolygonMode(GL_FRONT, GL_LINE);
		glEnable(GL_CULL_FACE);
		glCullFace(GL_BACK);
		origglDrawElements(mode, count, type, indices);
		//ColorFunc(0, 255, 0, 130);//green
		//ColorFunc(255, 0, 0, 130);//red
		ColorFunc(255, 0, 255, 130);//pink
		

		glDisableClientState(GL_TEXTURE_COORD_ARRAY);
		glEnable(GL_DEPTH_TEST);
		glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
		glPopMatrix();
	




		glPushMatrix();
		glDisable(GL_DEPTH_TEST);
		glScalef(1, 1, 1);
		ColorFunc(0, 255, 0, 140);//green
								  //ColorFunc(220, 0, 0, 140); //red
		origglDrawElements(mode, count, type, indices);
		glEnable(GL_TEXTURE_2D);
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glScalef(0.99, 1.01, 1.0);
		glEnable(GL_DEPTH_TEST);
		origglDrawElements(mode, count, type, indices);
		ColorFunc(20, 20, 20, 120);//ColorFunc(20, 20, 20, 120); //black
		glPopMatrix();
	}
	*/

	


	/*
	//chams wired -working
	if (free_for_all_player_models || red_team_player_models || blue_team_player_models)
	{
		glPushMatrix();
		DisableDepthTest();//glDisable(GL_DEPTH_TEST);
		glDisable(GL_TEXTURE_2D);
		//glEnable(GL_COLOR_MATERIAL);
		//glDisableClientState(GL_COLOR_ARRAY);
		ColorFunc(0, 0, 0, 255);//black
		glDisableClientState(GL_TEXTURE_COORD_ARRAY);
		glEnable(GL_POLYGON_OFFSET_LINE);
		glEnable(GL_LINE_LOOP);
		glLineWidth(2.5);
		glPolygonMode(GL_FRONT, GL_LINE);
		glEnable(GL_CULL_FACE);
		glCullFace(GL_BACK);
		//origglDrawElements(mode, count, type, indices);
		if (free_for_all_player_models)
			glColor4ub(255, 0, 255, 255); //behind walls (pink colour)
		else if (red_team_player_models)
			glColor4ub(255, 80, 0, 255); //behind walls (yellow colour)
		else if (blue_team_player_models)
			glColor4ub(255, 255, 255, 255); //behind walls (white colour)
		origglDrawElements(mode, count, type, indices);

		glDisableClientState(GL_TEXTURE_COORD_ARRAY);
		glEnable(GL_DEPTH_TEST);
		glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
		//origglDrawElements(mode, count, type, indices);



		//glEnable(GL_COLOR_MATERIAL);
		//glDisableClientState(GL_COLOR_ARRAY);
		glScalef(1, 1, 1);
		if (free_for_all_player_models)
			glColor4ub(38, 255, 38, 255); //infront of  walls (green colour)
		else if (red_team_player_models)
			glColor4ub(255, 0, 0, 255); //infront of walls (red colour)
		else if (blue_team_player_models)
			glColor4ub(0, 255, 255, 255); //infront of walls (blue colour)
		origglDrawElements(mode, count, type, indices);

		glEnable(GL_TEXTURE_2D);
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glScalef(0.99, 1.01, 1.0);
		EnableDepthTest();//glEnable(GL_DEPTH_TEST);
		origglDrawElements(mode, count, type, indices);
		ColorFunc(20, 20, 20, 120); //black
		glPopMatrix();
	}
	
	*/




	// call original
	(*origglDrawElements)(mode, count, type, indices);
}

// =============================================================================================== //


void APIENTRY newglVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer)
{
	


	// call original
	origglVertexPointer (size, type, stride, pointer);
}

// =============================================================================================== //


/*
//this swapbuffers code kind of works but makes game crash when pressing escape key to open Quake 3 menu when in gameplay.
bool contextCreated = false;
HGLRC myContext;
HGLRC gameContext;
HGLRC oldContext;


void __stdcall newwglSwapBuffers(HDC hDC)
{

	//Save the games context
	gameContext = wglGetCurrentContext();

	//Create our own context if it hasn't been created yet
	if (contextCreated == false)
	{
		//Create new context
		myContext = wglCreateContext(hDC);

		//Make thread use our context
		wglMakeCurrent(hDC, myContext);


		//Setup our context
		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		glOrtho(0.0, 1600, 1200, 0.0, 1.0, -1.0);  //might want to make these your actual screen resolution
		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity();
		glClearColor(0, 0, 0, 1.0);
	}

	//Make thread use our context
	wglMakeCurrent(hDC, myContext);


	//Draw something (a rectangle here)
	glColor3f(1.0f, 0, 0);
	glBegin(GL_QUADS);
	glVertex2f(0, 190.0f);
	glVertex2f(100.0f, 190.0f);
	glVertex2f(100.0f, 290.0f);
	glVertex2f(0, 290.0f);
	glEnd();
	


	//Make thread to use games context again
	//wglMakeCurrent(hDC, oldContext);
	wglMakeCurrent(hDC, gameContext);


	origwglSwapBuffers(hDC);
}
*/




/*
void __stdcall newwglSwapBuffers(HDC hDC) 
{
	
	void initialise_draw_text();
	{
		GL::Font glFont;

		HDC currentHDC = wglGetCurrentDC();

		if (!glFont.bBuilt || currentHDC != glFont.hdc)
		{
			glFont.Build(FONT_HEIGHT);
		}
		

		GL::SetupOrtho();
	
		
		//Draw here
		glFont.Print(100, 100, rgb::red, "Jesus Loves You All");
		GL::DrawFilledRect(300, 300, 200, 200, rgb::red);
		GL::DrawOutline(550, 400, 200, 200, 1, rgb::green);



	glBegin(GL_LINES);
	glVertex2i(0, 0);
	glVertex2i(500, 500);
	glEnd();


	glBegin(GL_LINES);
	glColor3f(1.0f, 0.0f, 0.0f); // RGB value
	glVertex2f(0, 0); // Line Origin (top left)
	glVertex2f(800, 600); // Line end
	glEnd();
	


	  //End draw here
	GL::RestoreGL();
}
	
	

	// call original
	origwglSwapBuffers(hDC);
}
*/




// so far below working great USE BELOW!!!
// =============================================================================================== //
// kutaQ3 hook - ImGui rendering (runs inside the hooked wglSwapBuffers)

// the menu window - called it "kutaQ3 hook"
void RenderKutaQ3Menu()
{
	ImGui::SetNextWindowSize(ImVec2(300, 185), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("kutaQ3 hook", &bMenuShown, ImGuiWindowFlags_NoCollapse))
	{
		ImGui::End();
		return;
	}

	ImGui::Text("Quake 3 OpenGL hook");
	ImGui::Separator();

	ImGui::Checkbox("Chams (wallhack)", &bChamsEnabled);
	if (bChamsEnabled)
	{
		ImGui::RadioButton("Solid", &iChamsStyle, 0);
		ImGui::SameLine();
		ImGui::RadioButton("Wireframe", &iChamsStyle, 1);
	}

	ImGui::Checkbox("Log player shaders (F10)", &bLogShaders);

	ImGui::Separator();
	ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Press INSERT to show/hide this menu.");
	ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.4f, 1.0f), "ImGui %s", IMGUI_VERSION);
	ImGui::End();
}

// window procedure hook: feeds input to ImGui and swallows it while the menu is open
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK kutaQ3WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	// give ImGui first pick at the message
	if (bImGuiReady)
		if (ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam) && bMenuShown)
			return 0;

	// INSERT toggles the menu
	if (uMsg == WM_KEYDOWN && wParam == VK_INSERT)
	{
		bMenuShown = !bMenuShown;
		return 0;
	}

	// while the menu is open, don't let the game react to mouse/keyboard input
	if (bMenuShown)
	{
		switch (uMsg)
		{
		case WM_MOUSEMOVE:
		case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
		case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
		case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
		case WM_MOUSEWHEEL:
		case WM_KEYDOWN: case WM_KEYUP:
		case WM_SYSKEYDOWN: case WM_SYSKEYUP:
		case WM_CHAR:
			return 0;
		}
	}

	return CallWindowProc(oGameWndProc, hWnd, uMsg, wParam, lParam);
}

// hooked wglSwapBuffers: the frame is already drawn when this runs, so this is
// where the "kutaQ3 hook" menu is rendered on top of the game scene.
BOOL WINAPI newwglSwapBuffers(HDC hDC)
{
	// SwapBuffers is the right place for ImGui: it runs once at the end of a presented
	// frame. If a driver/game path re-enters SwapBuffers, do not build another ImGui
	// frame inside the same call chain.
	static bool bInSwapBuffersHook = false;
	if (bInSwapBuffersHook)
		return origwglSwapBuffers ? origwglSwapBuffers(hDC) : FALSE;

	bInSwapBuffersHook = true;

	if (!bImGuiReady)
	{
		// ImGui init binds and uploads textures on the game's GL context - keep our hooks out of the way
		bInsideImgui = true;

		HWND hwnd = WindowFromDC(hDC);
		if (!hwnd) hwnd = GetForegroundWindow();
		g_GameHwnd = hwnd;

		// Dear ImGui context & style
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = NULL; // don't write imgui.ini next to the game
		io.LogFilename = NULL; // don't write imgui_log.txt next to the game
		// Cursor flicker fix: Quake 3 hides the OS mouse cursor during gameplay and draws
		// its own crosshair, while the ImGui Win32 backend would otherwise change the OS
		// cursor every frame. Leave OS cursor visibility entirely under Quake 3's control.
		io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
		ImGui::StyleColorsDark();

		// platform + renderer backends (the fixed function OpenGL2 backend fits Quake 3's legacy GL context)
		ImGui_ImplWin32_Init(hwnd);
		ImGui_ImplOpenGL2_Init();

		// subclass the game window so the menu receives mouse/keyboard input
		oGameWndProc = (WNDPROC)SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)kutaQ3WndProc);
		if (!oGameWndProc) oGameWndProc = DefWindowProc;

		bImGuiReady = true;
		bInsideImgui = false;
	}
	else
	{
		// the game may recreate its window (e.g. vid_restart) - rebind the input backend then
		HWND hwnd = WindowFromDC(hDC);
		if (hwnd && hwnd != g_GameHwnd)
		{
			if (oGameWndProc && IsWindow(g_GameHwnd))
				SetWindowLongPtr(g_GameHwnd, GWLP_WNDPROC, (LONG_PTR)oGameWndProc);
			g_GameHwnd = hwnd;
			bInsideImgui = true;
			ImGui_ImplWin32_Shutdown();
			ImGui_ImplWin32_Init(hwnd);
			oGameWndProc = (WNDPROC)SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)kutaQ3WndProc);
			bInsideImgui = false;
		}
	}

	// build the frame
	// (ImGui may (re)create its font texture during NewFrame, so the guard stays up for the whole frame)
	bInsideImgui = true;
	ImGui_ImplOpenGL2_NewFrame();
	ImGui_ImplWin32_NewFrame();
	// Draw ImGui's software cursor only while its menu is visible.
	ImGui::GetIO().MouseDrawCursor = bMenuShown;
	ImGui::NewFrame();

	if (bMenuShown)
		RenderKutaQ3Menu();

	ImGui::EndFrame();
	ImGui::Render();

	// draw the menu on top of the game scene, then let the game swap
	ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
	bInsideImgui = false;

	// call original
	BOOL result = origwglSwapBuffers ? origwglSwapBuffers(hDC) : FALSE;
	bInSwapBuffersHook = false;
	return result;

}



// =============================================================================================== //


HWND WINAPI newCreateWindowExA (
	_In_     DWORD     dwExStyle,
	_In_opt_ LPTSTR    lpClassName,
	_In_opt_ LPTSTR    lpWindowName,
	_In_     DWORD     dwStyle,
	_In_     int       x,
	_In_     int       y,
	_In_     int       nWidth,
	_In_     int       nHeight,
	_In_opt_ HWND      hWndParent,
	_In_opt_ HMENU     hMenu,
	_In_opt_ HINSTANCE hInstance,
	_In_opt_ LPVOID    lpParam
) {  
	

	if (lpWindowName && !strcmp(lpWindowName, "Quake 3 Console"))
	{
		//(void *)pVM_Create = detour(VM_CREATE_ADDR, (void *)&VM_Create, 0x8, 0);
		//(void *)pVM_Call = detour(VM_CALL_ADDR, (void *)&VM_Call, 0x8, 0);
		//(void *)pGetArg = detour(VM_GETARG_ADDR, (void *)&VM_GetArg, 0x6, 0);
		
		//MessageBox(NULL, "Hello World!", "Test", MB_OK);
	}


	 return origCreateWindowExA (dwExStyle, lpClassName, lpWindowName, dwStyle, x, y, nWidth, nHeight, hWndParent, hMenu, hInstance, lpParam);
}

// =============================================================================================== //


HMODULE WINAPI newLoadLibraryExA (
	_In_       LPCTSTR lpFileName,
	_Reserved_ HANDLE  hFile,
	_In_       DWORD   dwFlags
) {

	/*
	if (strstr(lpFileName, "cgame_mp_x86.dll"))

	{
		//do stuff
	}
	*/

	return origLoadLibraryExA (lpFileName, hFile, dwFlags);
}



// =============================================================================================== //


bool bGLSet = false; //hook and init once
void HookFunctions()
{
	//Initialise();
	HMODULE oMod = GetModuleHandle("opengl32.dll");
	HMODULE gMod = GetModuleHandle("gdi32.dll");
	HMODULE uMod = GetModuleHandle("User32.dll");
	HMODULE kMod = GetModuleHandle("kernel32.dll");


	if (!bGLSet)
	{
		if(oMod)
		{
			origglBindTexture = (glBindTexture_t)(DWORD)GetProcAddress(oMod, "glBindTexture");
			origglDrawElements = (glDrawElements_t)(DWORD)GetProcAddress(oMod, "glDrawElements");
			origglVertexPointer = (glVertexPointer_t)(DWORD)GetProcAddress(oMod, "glVertexPointer");

			DetourTransactionBegin();
			DetourUpdateThread(GetCurrentThread());
			DetourAttach(&(PVOID &)origglBindTexture, newglBindTexture);
			DetourAttach(&(PVOID &)origglDrawElements, newglDrawElements);
			DetourAttach(&(PVOID &)origglVertexPointer, newglVertexPointer);
			DetourTransactionCommit();
		}

		// Quake 3 presents frames through the Win32 GDI SwapBuffers(HDC) export.
		// Hooking this end-of-frame call renders ImGui once per presented frame.
		// Rendering ImGui from glDrawElements/glBegin would run once per draw call and
		// makes the menu appear to be drawn many times.
		if (gMod)
			origwglSwapBuffers = (SwapBuffers_t)(DWORD)GetProcAddress(gMod, "SwapBuffers");

		if (!origwglSwapBuffers && oMod)
		{
			// Fallback for wrappers that expose wglSwapBuffers from opengl32.dll.
			origwglSwapBuffers = (SwapBuffers_t)(DWORD)GetProcAddress(oMod, "wglSwapBuffers");
		}

		if (origwglSwapBuffers)
		{
			DetourTransactionBegin();
			DetourUpdateThread(GetCurrentThread());
			DetourAttach(&(PVOID &)origwglSwapBuffers, newwglSwapBuffers);
			DetourTransactionCommit();
		}

		if (uMod)
		{
			origCreateWindowExA = (CreateWindowExA_t)(DWORD)GetProcAddress(uMod, "CreateWindowExA");

			DetourTransactionBegin();
			DetourUpdateThread(GetCurrentThread());
			DetourAttach(&(PVOID &)origCreateWindowExA, newCreateWindowExA);
			DetourTransactionCommit();
		}


		if (kMod)
		{
			origLoadLibraryExA = (LoadLibraryExA_t)(DWORD)GetProcAddress(kMod, "LoadLibraryExA");

			DetourTransactionBegin();
			DetourUpdateThread(GetCurrentThread());
			DetourAttach(&(PVOID &)origLoadLibraryExA, newLoadLibraryExA);
			DetourTransactionCommit();
		}


	}
	bGLSet = true;
}
																																															
// =============================================================================================== //

BOOL WINAPI DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpvReserved)
{
	switch (dwReason)
	{
	case DLL_PROCESS_ATTACH:
		{
			DisableThreadLibraryCalls(hModule);
			GetModuleFileName(hModule, dlldir, 512);
			for (int i = strlen(dlldir); i > 0; i--) { if (dlldir[i] == '\\') { dlldir[i + 1] = 0; break; } }
			HookFunctions();
			break;
		}	

	case DLL_PROCESS_DETACH:
		{
			// shut down the kutaQ3 hook menu
			if (bImGuiReady)
			{
				if (oGameWndProc && IsWindow(g_GameHwnd))
					SetWindowLongPtr(g_GameHwnd, GWLP_WNDPROC, (LONG_PTR)oGameWndProc);
				if (wglGetCurrentContext() != NULL) // renderer teardown touches GL - needs a live context
					ImGui_ImplOpenGL2_Shutdown();
				ImGui_ImplWin32_Shutdown();
				ImGui::DestroyContext();
				bImGuiReady = false;
			}

			DetourTransactionBegin();
			DetourUpdateThread(GetCurrentThread());
			DetourDetach(&(PVOID &)origglBindTexture, newglBindTexture);
			DetourDetach(&(PVOID &)origglDrawElements, newglDrawElements);
			DetourDetach(&(PVOID &)origglVertexPointer, newglVertexPointer);
			if (origwglSwapBuffers)
				DetourDetach(&(PVOID &)origwglSwapBuffers, newwglSwapBuffers);
			DetourDetach(&(PVOID &)origCreateWindowExA, newCreateWindowExA);
			DetourDetach(&(PVOID &)origLoadLibraryExA, newLoadLibraryExA);

			DetourTransactionCommit();
			::FreeLibrary(hModule);
			hModule = 0;
			break;
		}
	}
	return TRUE;
}



