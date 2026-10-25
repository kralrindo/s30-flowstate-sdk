//=============================================================================//
//
// Purpose: ImGui panels of the S30 SDK overlay.
//
// Drawn inside the overlay's ImGui frame (render thread, under the ImGui lock):
// a top bar, the SDK console (the console ring + command input), a status
// panel and a connect panel.
//
//=============================================================================//
#include <windows.h>
#include <cstdio>
#include <cstring>
#include "thirdparty/imgui/imgui.h"
#include "s30_image.h"
#include "console_s30.h"
#include "net_guard_s30.h"
#include "overlay_s30.h"
#include "sdk_commands_s30.h"

static bool s_bShowConsole = true;
static bool s_bShowStatus = false;
static bool s_bShowConnect = false;

static char s_szInput[512] = {};
static char s_szFilter[128] = {};
static const int s_nHistory = 32;
static char s_szHistory[s_nHistory][512] = {};
static int s_nHistoryCount = 0;
static int s_nHistoryPos = -1;
static unsigned long long s_nSeenLines = 0;
static bool s_bScrollToBottom = true;
static char s_szConnectAddress[128] = "127.0.0.1:37015";

void S30Panels_Init(void)
{
	ImGuiStyle& style = ImGui::GetStyle();
	style.WindowRounding = 4.0f;
	style.FrameRounding = 3.0f;
	style.ScrollbarRounding = 3.0f;
}

void S30Panels_ToggleConsole(void)
{
	s_bShowConsole = !s_bShowConsole;
}

// Set whenever the overlay opens: the console input takes keyboard focus on the
// next frame (IsWindowAppearing alone misses an overlay that starts open).
static volatile bool s_bFocusInput = true;

void S30Panels_OnOpened(void)
{
	s_bFocusInput = true;
	s_bShowConsole = true;
}

static ImVec4 Panels_LineColor(S30ConsoleColor_t eColor)
{
	switch (eColor)
	{
	case S30ConsoleColor_t::SDK:     return ImVec4(0.45f, 0.85f, 1.00f, 1.0f);
	case S30ConsoleColor_t::WARNING: return ImVec4(1.00f, 0.85f, 0.35f, 1.0f);
	case S30ConsoleColor_t::FAULT:   return ImVec4(1.00f, 0.40f, 0.40f, 1.0f);
	case S30ConsoleColor_t::SCRIPT:  return ImVec4(0.55f, 0.95f, 0.55f, 1.0f);
	default:                         return ImVec4(0.90f, 0.90f, 0.90f, 1.0f);
	}
}

static void Panels_DrawLine(const char* pszLine, S30ConsoleColor_t eColor, void*)
{
	if (s_szFilter[0] && !strstr(pszLine, s_szFilter))
		return;
	ImGui::PushStyleColor(ImGuiCol_Text, Panels_LineColor(eColor));
	ImGui::TextUnformatted(pszLine);
	ImGui::PopStyleColor();
}

static void Panels_PushHistory(const char* pszLine)
{
	if (s_nHistoryCount && !strcmp(s_szHistory[(s_nHistoryCount - 1) % s_nHistory], pszLine))
		return;
	strncpy_s(s_szHistory[s_nHistoryCount % s_nHistory], pszLine, _TRUNCATE);
	++s_nHistoryCount;
}

static int Panels_InputCallback(ImGuiInputTextCallbackData* pData)
{
	if (pData->EventFlag == ImGuiInputTextFlags_CallbackHistory)
	{
		const int nAvailable = s_nHistoryCount < s_nHistory ? s_nHistoryCount : s_nHistory;
		if (!nAvailable)
			return 0;
		if (pData->EventKey == ImGuiKey_UpArrow)
			s_nHistoryPos = s_nHistoryPos < 0 ? 0 : (s_nHistoryPos + 1 < nAvailable ? s_nHistoryPos + 1 : s_nHistoryPos);
		else if (pData->EventKey == ImGuiKey_DownArrow)
			s_nHistoryPos = s_nHistoryPos > 0 ? s_nHistoryPos - 1 : -1;
		pData->DeleteChars(0, pData->BufTextLen);
		if (s_nHistoryPos >= 0)
			pData->InsertChars(0, s_szHistory[(s_nHistoryCount - 1 - s_nHistoryPos) % s_nHistory]);
	}
	else if (pData->EventFlag == ImGuiInputTextFlags_CallbackCompletion)
	{
		const char* ppszMatches[16];
		const int nMatches = S30Commands_Complete(pData->Buf, ppszMatches, 16);
		if (nMatches == 1)
		{
			pData->DeleteChars(0, pData->BufTextLen);
			pData->InsertChars(0, ppszMatches[0]);
			pData->InsertChars(pData->CursorPos, " ");
		}
		else if (nMatches > 1)
		{
			char szList[512] = {};
			for (int i = 0; i < nMatches; ++i)
			{
				strncat_s(szList, ppszMatches[i], _TRUNCATE);
				strncat_s(szList, "  ", _TRUNCATE);
			}
			strncat_s(szList, "\n", _TRUNCATE);
			S30Console_Write(szList, S30ConsoleColor_t::SDK);
		}
	}
	return 0;
}

static void Panels_DrawConsole(void)
{
	ImGui::SetNextWindowSize(ImVec2(900, 500), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowPos(ImVec2(40, 60), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("SDK Console", &s_bShowConsole))
	{
		ImGui::End();
		return;
	}

	ImGui::SetNextItemWidth(220.0f);
	ImGui::InputTextWithHint("##filter", "filter", s_szFilter, sizeof(s_szFilter));
	ImGui::SameLine();
	if (ImGui::Button("Clear"))
		S30Console_ClearLines();

	const float flFooter = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
	if (ImGui::BeginChild("##lines", ImVec2(0, -flFooter), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar))
	{
		const bool bAtBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f;
		const unsigned long long nTotal = S30Console_VisitLines(Panels_DrawLine, nullptr);
		if (nTotal != s_nSeenLines && (bAtBottom || s_bScrollToBottom))
			ImGui::SetScrollHereY(1.0f);
		s_nSeenLines = nTotal;
		s_bScrollToBottom = false;
	}
	ImGui::EndChild();

	ImGui::SetNextItemWidth(-1.0f);
	const ImGuiInputTextFlags nFlags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory
		| ImGuiInputTextFlags_CallbackCompletion;
	if (ImGui::InputTextWithHint("##input", "command (Tab completes SDK commands, Up/Down history)", s_szInput, sizeof(s_szInput),
		nFlags, Panels_InputCallback))
	{
		if (s_szInput[0])
		{
			Panels_PushHistory(s_szInput);
			S30Console_Submit(s_szInput);
		}
		s_szInput[0] = '\0';
		s_nHistoryPos = -1;
		s_bScrollToBottom = true;
		ImGui::SetKeyboardFocusHere(-1);
	}
	if (ImGui::IsWindowAppearing() || s_bFocusInput)
	{
		ImGui::SetKeyboardFocusHere(-1);
		s_bFocusInput = false;
	}
	ImGui::End();
}

static void Panels_DrawStatus(void)
{
	ImGui::SetNextWindowSize(ImVec2(520, 220), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("SDK Status", &s_bShowStatus))
	{
		ImGui::End();
		return;
	}
	const S30Image_t* const pImage = S30Image_Get();
	ImGui::Text("Host build: %s", pImage ? pImage->pszBuild : "(unknown)");
	ImGui::Text("Frames presented: %lld", S30Overlay_PresentCount());
	ImGui::Text("Network guard: %ld outbound attempts refused (loopback only)", S30NetGuard_BlockedCount());
	ImGui::Text("ImGui %s, %.0f fps (overlay)", IMGUI_VERSION, ImGui::GetIO().Framerate);
	ImGui::Separator();
	ImGui::TextWrapped("Offline platform patches, engine/script log capture and the console are active. "
		"See platform/logs/client_s30/ for the full log.");
	ImGui::End();
}

static void Panels_DrawConnect(void)
{
	ImGui::SetNextWindowSize(ImVec2(460, 150), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Connect", &s_bShowConnect))
	{
		ImGui::End();
		return;
	}
	ImGui::SetNextItemWidth(260.0f);
	ImGui::InputText("Address", s_szConnectAddress, sizeof(s_szConnectAddress));
	if (ImGui::Button("Connect"))
	{
		char szCommand[200];
		snprintf(szCommand, sizeof(szCommand), "connect %s", s_szConnectAddress);
		S30Console_Submit(szCommand);
	}
	ImGui::SameLine();
	if (ImGui::Button("Disconnect"))
		S30Console_Submit("disconnect");
	ImGui::TextWrapped("Native S30 connect. The S3 net bridge is not ported yet, so an S3 dedicated server will not accept it.");
	ImGui::End();
}

void S30Panels_Draw(void)
{
	if (ImGui::BeginMainMenuBar())
	{
		ImGui::TextColored(ImVec4(0.45f, 0.85f, 1.0f, 1.0f), "R5F S30 SDK");
		ImGui::Separator();
		ImGui::MenuItem("Console", nullptr, &s_bShowConsole);
		ImGui::MenuItem("Status", nullptr, &s_bShowStatus);
		ImGui::MenuItem("Connect", nullptr, &s_bShowConnect);
		ImGui::Separator();
		ImGui::TextDisabled("` / F10 closes");
		ImGui::EndMainMenuBar();
	}
	if (s_bShowConsole)
		Panels_DrawConsole();
	if (s_bShowStatus)
		Panels_DrawStatus();
	if (s_bShowConnect)
		Panels_DrawConnect();
}
