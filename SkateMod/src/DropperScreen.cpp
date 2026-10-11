#include "DropperScreen.h"

#include "DropperEditor.h"
#include "DropperInput.h"
#include "DropperSnap.h"
#include "DropperWorld.h"

#include <algorithm>
#include <array>
#include <functional>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <unordered_map>

using namespace SDK;

namespace DropperScreen
{
    namespace
    {
        using Apt::Args;
        using Apt::Value;

        // Same fixed rate as DropperEditor (the screen handlers and the editor run in one game update).
        constexpr double Step = 1.0 / 60.0;   // the editor's 60 Hz world tick (DropperEditor.cpp)

        // FEObjectDropperScreenMode_* (the movie's SetState argument): the index into the game's
        // table of 30 names at 0x82214e0c, sent with the movie vtable +0x14.
        enum Msg : int
        {
            FreeCam, Catalog, Manipulate, Refresh, NextCategory, PreviousCategory, NextSubCategory, PreviousSubCategory,
            NextItem, PreviousItem, NextType, PreviousType, SnapObject, MoveOnLockedAxis, RotateObject, GroupSelect,
            HideSelection, ShowSelection, Delete, RefreshFengShui, EnableUI, DisableUI, RefreshDuplicate, RefreshFineTune,
            RefreshSize, ShowLockAxis, ShowRotate, RefreshInfo, GotoSubCategories, GotoItems, MsgCount
        };
        const char* const MsgNames[MsgCount] = {
            "FreeCam", "Catalog", "Manipulate", "Refresh", "NextCategory", "PreviousCategory", "NextSubCategory",
            "PreviousSubCategory", "NextItem", "PreviousItem", "NextType", "PreviousType", "SnapObject", "MoveOnLockedAxis",
            "RotateObject", "GroupSelect", "HideSelection", "ShowSelection", "Delete", "RefreshFengShui", "EnableUI",
            "DisableUI", "RefreshDuplicate", "RefreshFineTune", "RefreshSize", "ShowLockAxis", "ShowRotate", "RefreshInfo",
            "GotoSubCategories", "GotoItems" };

        using DropperInput::AptEvent;

        // ---------------------------------------------------------------- catalog (8285a200)

        int Stricmp(const std::string& A, const std::string& B) { return _stricmp(A.c_str(), B.c_str()); }

        // FUN_82858640: the size of a piece from its name "xx_<base>_<size>_...": sm/sml 1, md/med 2, lg/lrg 3.
        int SizeOf(const std::string& Name)
        {
            const size_t A = Name.find('_');
            if (A != 2) return 0;
            const size_t B = Name.find('_', A + 1);
            if (B == std::string::npos) return 0;
            size_t C = Name.find('_', B + 1);
            if (C == std::string::npos) C = Name.size();
            const std::string T = Name.substr(B + 1, C - B - 1);
            if (!Stricmp(T, "sm") || !Stricmp(T, "sml")) return 1;
            if (!Stricmp(T, "md") || !Stricmp(T, "med")) return 2;
            if (!Stricmp(T, "lg") || !Stricmp(T, "lrg")) return 3;
            return 0;
        }

        // fe_park_itemtypes records with their match text (field E110094B), in record order.
        const char* const Styles[3] = { "downtown", "industrial", "university" };
        // FUN_82858418: the style is the text after the last '_' (-1 = none of the item types).
        int StyleOf(const std::string& Name)
        {
            const size_t U = Name.rfind('_');
            const std::string T = U == std::string::npos ? Name : Name.substr(U + 1);
            for (int i = 0; i < 3; i++)
                if (!Stricmp(T, Styles[i])) return i;
            return -1;
        }

        // fe_park_categories (skatercollections.vlt): supers terrain1 (10) / terrain2 (20) / dmo1 (30),
        // sub categories by sort order (EBF090FD), name halid, catalog match text E110094B.
        struct SubDef { const char* Halid; int Sort; };
        struct SuperDef { const char* Halid; int Sort; std::vector<SubDef> Subs; };
        const std::vector<SuperDef> Tree = {
            { "terrain", 10, { { "premade", 100 }, { "street", 200 }, { "vert", 300 }, { "plaza", 400 }, { "mega", 500 }, { "rails", 600 }, { "hubbas", 700 } } },
            { "miscterrain", 20, { { "branded", 100 }, { "downtown", 200 }, { "foliage", 300 }, { "misc", 400 }, { "ots", 500 }, { "teleports", 600 } } },
            { "props", 30, { { "dmo_barriers", 100 }, { "dmo_bins", 200 }, { "dmo_misc", 300 }, { "dmo_boxes", 400 }, { "dmo_rails", 500 }, { "dmo_ramps", 600 }, { "dmo_tables", 700 } } },
        };

        struct Sub { std::string Halid; std::vector<const DropperData::CatalogEntry*> Items; };
        struct Super { std::string Halid; bool Terrain = false; std::vector<Sub> Subs; };

        struct CatalogDb
        {
            std::vector<Super> Supers;
            int Size = 1;      // +0x100 (82859630: 1)
            int Style = 0;     // +0xf8 (82859630: item type 0)

            // 8285a200: every catalog object in its sub category when its style and size pass the
            // filters (a piece without a size / style always passes), sorted by name; empty sub
            // categories and super categories are dropped.
            void Build()
            {
                Supers.clear();
                const auto& All = DropperWorld::Library().Catalog();
                for (const SuperDef& Sd : Tree)
                {
                    Super S;
                    S.Halid = Sd.Halid;
                    S.Terrain = Sd.Sort != 30;
                    for (const SubDef& Bd : Sd.Subs)
                    {
                        Sub B;
                        B.Halid = Bd.Halid;
                        for (const auto& E : All)
                        {
                            if (Stricmp(E.Category, Bd.Halid)) continue;
                            const int St = StyleOf(E.Name), Sz = SizeOf(E.Name);
                            if (St != -1 && St != Style) continue;
                            if (Size != 0 && Sz != 0 && Sz != Size) continue;
                            B.Items.push_back(&E);
                        }
                        std::stable_sort(B.Items.begin(), B.Items.end(), [](const DropperData::CatalogEntry* L, const DropperData::CatalogEntry* R)
                        {
                            return Stricmp(L->Name, R->Name) < 0;
                        });
                        if (!B.Items.empty()) S.Subs.push_back(std::move(B));
                    }
                    if (!S.Subs.empty()) Supers.push_back(std::move(S));
                }
            }
        };

        // ---------------------------------------------------------------- state

        std::function<void(const std::string&)> LogFn;
        std::function<void(const char*)> SoundFn;
        std::string Root;
        std::unordered_map<std::string, std::string> Strings;

        std::unique_ptr<Apt::Player> Movie;
        Value Screen;
        std::vector<std::string> IconPaths;
        std::unordered_map<std::string, int> IconIndex;

        CatalogDb Db;
        bool bOpen = false, bClosed = false;
        double Accum = 0;
        UWorld* CurWorld = nullptr;

        // Screen object fields (offsets in the original)
        int State = 0;                  // +0x44
        bool bSelectingSub = true;      // +0x2cf: catalog on the sub category list (else the item list)
        bool bDirty = false;            // +0x2bc
        bool bItemReady = false;        // +0x200 (SkatePark.ItemSelectionReady)
        bool bUIEnabled = true;         // +0x228
        bool bCursorVisible = true;     // +0x2ce
        bool bDuplicate = false;        // +0x2cc (LB in free camera)
        bool bFineTune = false;         // +0x2cd
        int LastHover = -1;             // +0x2d0: the hovered object the prompts were last set for
        bool bLbHeld = false;           // +0x22a (action 0x1d TopLeftShoulder)
        bool bAPressed = false;         // +0x20c
        int AHeldMs = 0;                // +0x208
        bool bYRepeat = false;          // +0x22b
        int YCount = 0, YInterval = 0;  // +0x2b4 / +0x2b8
        int ShiftMode = 0;              // +0x2b0: 1 RB (lock axis) / 2 RT (rotate) shown
        int HeldMode = 2;               // +0x204: 0 rotate, 1 lock axis, 2 move
        int InputBlockMs = 0;           // +0x2c8
        bool bPickedUp = false;         // +0x214: holding an object picked up from the world

        // Catalog cursor (+0x50 block)
        int SuperIx = 0, SubIx = 0, ItemIx = 0;       // +0x68 / +0x70 / +0x78
        std::vector<int> SubItem;                      // +0x84: last item per sub category
        int Preview = 0;                               // the catalog's object at the cursor (825b9088)
        std::string PreviewId;
        double MsCarry = 0;   // Update: sub-millisecond rest of the fixed steps

        // Held object's pose before it was picked up (82866988 puts it back on cancel).
        FVector PickPos{};
        DropperWorld::Quat PickRot{};

        // ---- undo / redo (DAT_8300b290 + 600: a deque of at most 6 park snapshots, index +0x34) ----
        std::vector<DropperWorld::Snapshot> History;
        size_t HistoryIx = 0;
        const size_t HistoryMax = 6;

        // 825025F8: save the park. At the end: a new snapshot (the oldest goes past 6); after an
        // undo: the snapshots after the current one are dropped instead.
        void HistoryPush()
        {
            if (HistoryIx == History.size())
            {
                if (History.size() == HistoryMax) History.erase(History.begin());
                History.push_back(DropperWorld::Save());
            }
            else
            {
                HistoryIx++;
                while (HistoryIx < History.size()) History.pop_back();
            }
            HistoryIx = History.size();
        }
        // 82502878: the last snapshot taken goes (a property change was cancelled).
        void HistoryDropLast()
        {
            if (History.empty()) return;
            History.pop_back();
            HistoryIx = std::min(HistoryIx, History.size());
        }
        bool CanUndo() { return !History.empty() && HistoryIx != 0 && HistoryIx <= History.size(); }   // 825B7EC8 item 6
        bool CanRedo() { return HistoryIx + 1 < History.size(); }                                     // item 7
        // 82502948: back to the snapshot before the current one (Keep: without saving the present first).
        void Undo(bool bKeep)
        {
            if (!CanUndo()) return;
            size_t LoadIx = HistoryIx - 1;
            const DropperWorld::Snapshot S = History[LoadIx];
            if (!bKeep && HistoryIx == History.size())
            {
                if (History.size() == HistoryMax && LoadIx > 0) LoadIx--;   // the oldest goes
                HistoryPush();   // the present, for a redo
            }
            // the loaded snapshot becomes the current one
            HistoryIx = LoadIx;
            DropperWorld::Load(CurWorld, S);
        }
        // 82502C10
        void Redo()
        {
            if (HistoryIx >= History.size()) return;
            HistoryIx++;
            if (HistoryIx < History.size()) DropperWorld::Load(CurWorld, History[HistoryIx]);
        }

        // ---- group select (the cursor's group, cursor +4; painted by 825B00B8) ----
        std::vector<int> Group;
        bool bGroupMode = false;        // editor +0x55e
        int GroupPaint = 0;             // +0x2dc: 1 A held (add), 2 Y held (remove)
        void GroupHighlight(int H, bool bOn) { DropperWorld::SetDisplayMode(H, bOn ? 1 : 0); }
        void GroupClear()   // 82863498: the members lose their highlight, the group goes
        {
            for (int H : Group) GroupHighlight(H, false);
            Group.clear();
            bGroupMode = false;
            GroupPaint = 0;
        }

        void Log(const std::string& S) { if (LogFn) LogFn(S); }

        // A button glyph by its Skate 3 name ("B_hud", "L_Bumper_hud"): engine drawn in Skate 3 (the
        // movie only names it: button_item2 sets mButtonRender._test, texts use "<B:name>"); here
        // Skate 3's own miscboot button art (skate-data/hud/buttons/<name>.png). -1 = none.
        int IconFor(const std::string& Name)
        {
            auto It = IconIndex.find(Name);
            if (It == IconIndex.end())
            {
                const std::string Path = Root + "\\hud\\buttons\\" + Name + ".png";
                int Ix = -1;
                if (FILE* F = nullptr; fopen_s(&F, Path.c_str(), "rb") == 0 && F) { fclose(F); Ix = int(IconPaths.size()); IconPaths.push_back(Path); }
                else if (LogFn) LogFn("[dropper] no button art for " + Name);
                It = IconIndex.emplace(Name, Ix).first;
            }
            return It->second;
        }
        void Sound(const char* Cue) { if (SoundFn) SoundFn(Cue); }

        std::string Localize(const std::string& Key)
        {
            auto It = Strings.find(Key);
            if (It != Strings.end()) return It->second;
            std::string Up = Key;
            for (char& C : Up) C = char(toupper((unsigned char)C));
            It = Strings.find(Up);
            return It != Strings.end() ? It->second : std::string();
        }
        std::string LocalizeOr(const std::string& Key, const std::string& Missing)
        {
            const std::string S = Localize(Key);
            return S.empty() ? Missing : S;
        }

        void Send(int M)
        {
            if (Movie && Screen.t == Value::Obj) Movie->CallMethod(Screen, "SetState", { Value::Number(M) });
        }

        // ---- catalog cursor (825b8b00 / 8d10 / 8dc8 / 8e88 / 8f18 / 8fb0 / 9018 / 9088) ----
        const Super* CurSuper() { return SuperIx >= 0 && SuperIx < int(Db.Supers.size()) ? &Db.Supers[size_t(SuperIx)] : nullptr; }
        const Sub* CurSub()
        {
            const Super* S = CurSuper();
            return S && SubIx >= 0 && SubIx < int(S->Subs.size()) ? &S->Subs[size_t(SubIx)] : nullptr;
        }
        const DropperData::CatalogEntry* CurItem()
        {
            const Sub* B = CurSub();
            return B && ItemIx >= 0 && ItemIx < int(B->Items.size()) ? B->Items[size_t(ItemIx)] : nullptr;
        }
        int NumSupers() { return int(Db.Supers.size()); }
        int NumSubs() { const Super* S = CurSuper(); return S ? int(S->Subs.size()) : 0; }
        int NumItems() { const Sub* B = CurSub(); return B ? int(B->Items.size()) : 0; }

        // 825b8b00 / 825b8ba0: counts for the current position, indices kept in range.
        void Recount()
        {
            SuperIx = std::clamp(SuperIx, 0, std::max(0, NumSupers() - 1));
            SubItem.resize(size_t(std::max(1, NumSubs())), 0);
            SubIx = std::clamp(SubIx, 0, std::max(0, NumSubs() - 1));
            ItemIx = std::clamp(ItemIx, 0, std::max(0, NumItems() - 1));
            for (int& I : SubItem) I = std::max(0, I);
        }

        void RemovePreview()
        {
            if (Preview)
            {
                DropperWorld::Remove(Preview);
                DropperEditor::CursorToCamera();   // 82866988
            }
            Preview = 0;
            PreviewId.clear();
        }

        // 825b9088: the current item becomes the catalog's object at the cursor.
        void UpdatePreview()
        {
            const DropperData::CatalogEntry* E = CurItem();
            if (!E) { RemovePreview(); return; }
            if (Preview && PreviewId == E->Id) return;
            RemovePreview();
            // 825b91d0: spawned at the cursor, moved to camera eye + forward * camera distance, with
            // the cursor's rotation (828626e0); the catalog camera looks at it from where it is.
            const FVector At = DropperEditor::SpawnCursor();
            Preview = DropperWorld::Add(CurWorld, E->Id, At, DropperEditor::SpawnRotation(), true);
            PreviewId = Preview ? E->Id : std::string();
        }

        void SelectSuper(int Ix)   // 825b8d10 / 825b8dc8: sub 0, item 0
        {
            SuperIx = Ix;
            SubIx = 0;
            ItemIx = 0;
            SubItem.assign(size_t(std::max(1, NumSubs())), 0);
            Recount();
            Sound("sp_catalog_vert_scroll");
        }
        void SelectSub(int Ix)     // 825b8e88 / 825b8f18: the sub category's remembered item
        {
            SubIx = Ix;
            ItemIx = SubIx < int(SubItem.size()) ? SubItem[size_t(SubIx)] : 0;
            Recount();
            Sound("sp_catalog_vert_scroll");
        }
        void SelectItem(int Ix)    // 825b8fb0 / 825b9018
        {
            ItemIx = Ix;
            if (SubIx < int(SubItem.size())) SubItem[size_t(SubIx)] = ItemIx;
            UpdatePreview();
            Sound("sp_catalog_horz_scroll");
        }

        // 825b9550 + 825b4610: X cycles the catalog's size filter, then the catalog is rebuilt.
        void CycleSize()
        {
            if (Db.Size != 0 && Db.Size != 4) Db.Size = Db.Size == 3 ? 1 : Db.Size + 1;
            Db.Build();
            Recount();
            UpdatePreview();
            Sound("sp_catalog_horz_scroll");
        }

        // 8285b6f8: the catalog moves to an object's entry (and takes its size / style).
        bool FindInCatalog(const std::string& Id)
        {
            const DropperData::CatalogEntry* E = DropperWorld::Library().Find(Id);
            if (!E) return false;
            const int Sz = SizeOf(E->Name), St = StyleOf(E->Name);
            if (Sz != 0) Db.Size = Sz;
            if (St != -1) Db.Style = St;
            if (Sz != 0 || St != -1) Db.Build();
            for (size_t s = 0; s < Db.Supers.size(); s++)
                for (size_t b = 0; b < Db.Supers[s].Subs.size(); b++)
                    for (size_t i = 0; i < Db.Supers[s].Subs[b].Items.size(); i++)
                        if (Db.Supers[s].Subs[b].Items[i]->Id == Id)
                        {
                            SuperIx = int(s);
                            SubItem.assign(Db.Supers[s].Subs.size(), 0);
                            SubIx = int(b);
                            ItemIx = int(i);
                            SubItem[b] = ItemIx;
                            return true;
                        }
            return false;
        }

        // ---- editor ----
        // FUN_82864d68 key per screen state; FUN_82864ba8 mode.
        void EditorFor(int St)
        {
            switch (St)
            {
            case 1: case 4:
                DropperEditor::SetMode(DropperEditor::FreeCam);
                DropperEditor::SetTuning(DropperEditor::FreeCamTuning);
                break;
            case 3:
                DropperEditor::SetMode(DropperEditor::FreeCam);
                DropperEditor::SetTuning(DropperEditor::Manipulate);
                break;
            default:
                DropperEditor::SetMode(DropperEditor::Off);
                DropperEditor::SetTuning(DropperEditor::Default);
                break;
            }
        }

        // 82866988: cancel the carried object (a new one is removed, a picked up one goes back).
        void CancelHeld()
        {
            const std::vector<int> Others = DropperEditor::HeldGroup();
            const int H = DropperEditor::Release();
            if (!H) return;
            if (H == Preview) { Preview = 0; PreviewId.clear(); }
            DropperEditor::CursorToCamera();   // 82866988
            // 825b1d80 B: 82866988 removes what was carried; picked up from the world (+0x214),
            // 82502948(.., 1) loads the park saved at the pick-up (it goes back where it was)
            if (bPickedUp && CanUndo())
            {
                DropperWorld::Remove(H);
                for (int O : Others) DropperWorld::Remove(O);
                Undo(true);
                return;
            }
            if (bPickedUp) { DropperWorld::Move(H, PickPos, PickRot); return; }
            DropperWorld::Remove(H);
            for (int O : Others) DropperWorld::Remove(O);
        }

        void Close();
        void OpenProperties();

        // FUN_825b40f0
        void SetStateTo(int New)
        {
            if (State == New) return;
            const int Old = State;
            InputBlockMs = 0;
            // leaving the catalog (825b2c2c): its object at the cursor goes (82866988 via 825b8cb0)
            if (Old == 2 && New != 3) RemovePreview();
            if (Old == 2) DropperEditor::SetCatalogCamera(false);   // 825b6090
            State = New;
            if (Old == 4 && New != 3 && New != 6) GroupClear();
            switch (New)
            {
            case 0:
                EditorFor(0);
                break;
            case 1:
                AHeldMs = 0;
                LastHover = -1;   // +0x2d0 = -1
                Send(FreeCam);
                EditorFor(1);
                break;
            case 2:
                bDirty = true;
                EditorFor(2);
                DropperEditor::SetCatalogCamera(true);   // 825b5ed4
                Recount();
                Sound("sp_catalog");
                if (bSelectingSub) RemovePreview();
                else UpdatePreview();
                Send(Catalog);
                break;
            case 3:
                ShiftMode = 0;
                Send(Manipulate);
                EditorFor(3);
                break;
            case 4:
                EditorFor(4);
                bGroupMode = true;
                if (Old != 6)
                {
                    Send(GroupSelect);
                    Sound("sp_group_select");
                }
                break;
            default:
                if (bDuplicate) { bDuplicate = false; Send(RefreshDuplicate); }
                EditorFor(0);
                break;
            }
        }

        // 825b3040: leave the dropper.
        void Exit()
        {
            Sound("sp_object_dropper_exit");
            if (DropperEditor::Held()) CancelHeld();
            RemovePreview();
            SetStateTo(0);
            Close();
        }

        // Take an object into the hand: the editor carries it, the screen goes to state 3.
        void HoldObject(int Handle, bool bFromWorld, bool bFrame = true)
        {
            if (const DropperWorld::Placed* P = DropperWorld::Get(Handle))
            {
                PickPos = P->Position;
                PickRot = P->Rotation;
            }
            bPickedUp = bFromWorld;
            DropperEditor::Hold(Handle, bFrame);
            SetStateTo(3);
        }

        // 825b56b8: there is something to copy (the hovered object, else the carried one).
        bool CanDuplicate() { return DropperEditor::Hovered() != 0 || DropperEditor::Held() != 0; }

        // 82866848: a copy of the carried / hovered object, carried.
        int Duplicate(int Of)
        {
            const DropperWorld::Placed* P = DropperWorld::Get(Of);
            if (!P) return 0;
            return DropperWorld::Add(CurWorld, P->Id, P->Position, P->Rotation, true);
        }

        // ---------------------------------------------------------------- input handlers

        // FUN_825b3718: free camera.
        void FreeCamEvent(int E)
        {
            using namespace DropperInput;
            if (E == AptLShift1 && CanDuplicate())   // 825b3718 event 9 (825b56b8)
            {
                Sound("sp_l1_options");
                bDuplicate = true;
                Send(RefreshDuplicate);
            }
            else if (E == AptLShift1Released && bDuplicate)
            {
                bDuplicate = false;
                Send(RefreshDuplicate);
            }
            if (E == AptStart)   // not online: leave with the flicker sound
            {
                Sound("online_flicker");
                Exit();
                return;
            }
            if (E == AptPrevious && InputBlockMs == 0) { Exit(); return; }
            if (E == AptSelect) { OpenProperties(); return; }   // 825b1b90
            if (E == AptMisc)
            {
                if (!DropperEditor::Hovered() || !bLbHeld) SetStateTo(2);
                return;
            }
            if (E == AptNext) { bAPressed = true; return; }
            if (E == AptOptionReleased) { bYRepeat = false; Send(FreeCam); return; }
            if (E == AptOption)
            {
                if (DropperEditor::Hovered())
                {
                    if (!bYRepeat) { HistoryPush(); bYRepeat = true; YInterval = 60; YCount = 0; }   // 825B4034
                    const int H = DropperEditor::Hovered();
                    DropperWorld::Remove(H);   // 82866db0
                    Send(Delete);
                    Sound("sp_object_delete");
                    Send(RefreshFengShui);
                }
                return;
            }
            if (E == AptNextReleased && bAPressed)
            {
                bAPressed = false;
                AHeldMs = 0;
                const int H = DropperEditor::Hovered();
                if (!H) return;
                if (!bLbHeld)
                {
                    HistoryPush();   // 825B3DF8
                    Sound("sp_object_pickup");
                    HoldObject(H, true);
                }
                else if (const int Copy = Duplicate(H))
                {
                    Sound("sp_duplicate");
                    HoldObject(Copy, false);
                }
            }
        }

        // FUN_825b2820: catalog.
        void CatalogEvent(int E)
        {
            using namespace DropperInput;
            if (E == AptSelect)   // the properties of the item at the cursor (8285bb58 + 825b1b90)
            {
                if (!bSelectingSub && Preview) OpenProperties();
                return;
            }
            if (E == AptPrevious)
            {
                Sound("sp_object_cancel");
                if (!bSelectingSub)
                {
                    RemovePreview();
                    bSelectingSub = true;
                    Send(GotoSubCategories);
                    return;
                }
                SetStateTo(1);
                return;
            }
            if (E == AptNext)
            {
                if (bSelectingSub)
                {
                    Sound("core_a_button");
                    bSelectingSub = false;
                    UpdatePreview();
                    Send(GotoItems);
                    return;
                }
                // all items are unlocked here (8285bb58), so no lock / budget popups
                if (!Preview) { Sound("core_error"); return; }
                const int H = Preview;   // the catalog object is already at the cursor (828626e0): carried as it is
                Preview = 0;
                PreviewId.clear();
                Sound("sp_catalog_select");
                HoldObject(H, false, false);
                return;
            }
            if (State != 2) return;
            switch (E)
            {
            case AptMisc:   // X: the next size of a sized piece
                if (!bSelectingSub && CurItem() && SizeOf(CurItem()->Name) != 0)
                {
                    bDirty = true;
                    CycleSize();
                    Send(RefreshSize);
                }
                break;
            case AptRShift2:   // RT: next super category
                if (NumSupers() > 1)
                {
                    bDirty = true;
                    SelectSuper(SuperIx + 1 == NumSupers() ? 0 : SuperIx + 1);
                    if (!bSelectingSub) { bSelectingSub = true; RemovePreview(); }
                    Send(NextCategory);
                }
                break;
            case AptLShift2:   // LT: previous super category
                if (NumSupers() > 1)
                {
                    bDirty = true;
                    SelectSuper(SuperIx == 0 ? NumSupers() - 1 : SuperIx - 1);
                    if (!bSelectingSub) { bSelectingSub = true; RemovePreview(); }
                    Send(PreviousCategory);
                }
                break;
            case AptUp:
                bDirty = true;
                if (!bSelectingSub)
                {
                    SelectItem(ItemIx == 0 ? NumItems() - 1 : ItemIx - 1);
                    Send(PreviousItem);
                }
                else
                {
                    SelectSub(SubIx == 0 ? NumSubs() - 1 : SubIx - 1);
                    Send(PreviousSubCategory);
                }
                break;
            case AptDown:
                if (!bSelectingSub)
                {
                    if (NumItems() < 2) break;
                    bDirty = true;
                    SelectItem(ItemIx + 1 >= NumItems() ? 0 : ItemIx + 1);
                    Send(NextItem);
                }
                else
                {
                    bDirty = true;
                    SelectSub(SubIx + 1 == NumSubs() ? 0 : SubIx + 1);
                    Send(NextSubCategory);
                }
                break;
            default:
                break;
            }
        }

        // FUN_825b1d80: carrying an object.
        // 825b1c38 (from 825b1d80): carrying, LB shows duplicate (place and keep a copy) and LT fine tune.
        void HoldingShiftEvent(int E)
        {
            using namespace DropperInput;
            if (E == AptLShift1)
            {
                if (!CanDuplicate()) { bDuplicate = false; return; }
                Sound("sp_l1_options");
                bDuplicate = true;
                Send(RefreshDuplicate);
            }
            else if (E == AptLShift2)
            {
                Sound("sp_r1_options");   // the game's own cue for LT here
                bFineTune = true;
                Send(RefreshFineTune);
            }
            else if (E == AptLShift1Released && bDuplicate) { bDuplicate = false; Send(RefreshDuplicate); }
            else if (E == AptLShift2Released && bFineTune) { bFineTune = false; Send(RefreshFineTune); }
        }

        void HoldingEvent(int E)
        {
            using namespace DropperInput;
            HoldingShiftEvent(E);
            // RB / RT show their option (lock axis / rotate) while held
            if (E == AptLShift1 || E == AptRShift1 || E == AptLShift2 || E == AptRShift2)
            {
                if (ShiftMode == 0)
                {
                    if (E == AptRShift1) { Sound("sp_r1_options"); ShiftMode = 1; Send(ShowLockAxis); }
                    else if (E == AptRShift2) { ShiftMode = 2; Send(ShowRotate); }
                }
            }
            else if (E == AptRShift1Released) { if (ShiftMode == 1) { ShiftMode = 0; Send(Manipulate); } }
            else if (E == AptRShift2Released && ShiftMode == 2) { ShiftMode = 0; Send(Manipulate); }

            const bool bRb = E == AptRShift1, bRt = E == AptRShift2;
            const bool bX = E == AptMisc && !bRb && !bRt && ShiftMode == 0;
            // editor mode follows RB / RT (+0x204)
            if ((HeldMode == 0 && E == AptRShift2Released) || (HeldMode == 1 && E == AptRShift1Released))
            {
                Send(Manipulate);
                DropperEditor::SetMode(DropperEditor::FreeCam);
                HeldMode = 2;
            }
            else if (HeldMode == 2)
            {
                if (bRb)
                {
                    Sound("sp_l2_options");
                    Send(MoveOnLockedAxis);
                    DropperEditor::SetMode(DropperEditor::LockAxis);
                    HeldMode = 1;
                }
                else if (bRt)
                {
                    Sound("sp_r2_options");
                    Send(RotateObject);
                    DropperEditor::SetMode(DropperEditor::Rotate);
                    HeldMode = 0;
                }
            }

            if (E == AptPrevious && InputBlockMs == 0)   // B: cancel
            {
                Sound("sp_object_cancel");
                CancelHeld();
                bPickedUp = false;
                SetStateTo(1);
                Send(RefreshFengShui);
                return;
            }
            if (E == AptOption)   // Y: delete the carried object (82866988: its group too)
            {
                const std::vector<int> Others = DropperEditor::HeldGroup();
                const int H = DropperEditor::Release();
                if (!H) return;
                DropperWorld::Remove(H);
                for (int O : Others) DropperWorld::Remove(O);
                DropperEditor::CursorToCamera();   // 82866988
                Sound("sp_object_delete");
                SetStateTo(1);
                Send(Delete);
                return;
            }
            if (bX)   // X: back to the catalog at the carried object's entry (it is removed)
            {
                const int H = DropperEditor::Held();
                if (const DropperWorld::Placed* P = DropperWorld::Get(H))
                    if (FindInCatalog(P->Id)) bSelectingSub = false;
                const std::vector<int> Others = DropperEditor::HeldGroup();
                DropperEditor::Release();
                DropperWorld::Remove(H);
                for (int O : Others) DropperWorld::Remove(O);
                DropperEditor::CursorToCamera();   // 82866988 (825b2820)
                SetStateTo(2);
                return;
            }
            if (E == AptLShift3)   // left stick click: reset rotation (825b1d80 event 0xb)
            {
                if (DropperEditor::Held()) DropperEditor::ResetRotation();
                Sound("sp_reset_object");
                return;
            }
            if (E == AptSelect) { OpenProperties(); return; }   // 825b1d80 -> 825b1b90
            if (E != AptNext) return;
            // A: place (82868570 budget / 828668d8 placement)
            const int H = DropperEditor::Held();
            if (!H) return;
            if (!DropperEditor::HeldValid()) { Sound("sp_invalid_placement"); return; }   // 828668d8 false
            DropperEditor::Release();
            DropperEditor::Placed(H);   // 828668d8: its rotation kept, cursor back to the eye
            bPickedUp = false;
            Sound("sp_object_drop");
            if (bLbHeld)   // LB held: keep placing copies
                if (const int Copy = Duplicate(H))
                {
                    // 82866848: the cursor goes to eye + forward * distance (where the orbit had
                    // it) and the copy is carried there, without the pick-up zoom.
                    DropperEditor::SpawnCursor();
                    HoldObject(Copy, false, false);
                    Send(Manipulate);
                    return;
                }
            SetStateTo(1);
        }

        // FUN_825b00b8: the per-update part (A held -> group select, Y repeat delete, LB state).
        void Tick(int Ms)
        {
            if (State == 1)
            {
                // 825b00b8: the hovered object changed: the prompts follow it, and with LB held the
                // duplicate option lights for it
                const int Hov = DropperEditor::Hovered();
                if (Hov != LastHover)
                {
                    LastHover = Hov;
                    ShiftMode = 0;
                    if (!Hov) { bDuplicate = false; Send(HideSelection); }
                    else
                    {
                        Send(ShowSelection);
                        const bool bLb = DropperInput::TopLeftShoulderHeld();
                        bDuplicate = bLb && CanDuplicate();
                        Send(RefreshDuplicate);
                    }
                }
                if (!bYRepeat)
                {
                    if (bAPressed && (AHeldMs += Ms) > 200)
                    {
                        AHeldMs = 0;
                        bAPressed = false;
                        SetStateTo(4);
                        return;
                    }
                }
                else if (++YCount >= YInterval && DropperEditor::Hovered())
                {
                    YCount = 0;
                    if (YInterval > 0) YInterval /= 2;
                    DropperWorld::Remove(DropperEditor::Hovered());
                    Sound("sp_object_delete");
                    Send(RefreshFengShui);
                }
            }
            else
            {
                bYRepeat = false;
                bAPressed = false;
                AHeldMs = 0;
            }
            bLbHeld = DropperInput::TopLeftShoulderHeld();
            if (InputBlockMs) InputBlockMs = std::max(0, InputBlockMs - Ms);
        }

        // Group select (state 4, FUN_825b1428).
        void GroupEvent(int E)
        {
            using namespace DropperInput;
            if (E == AptPrevious && InputBlockMs == 0) { Sound("sp_object_cancel"); SetStateTo(1); return; }
            if (E == AptOption) { Sound("core_y_button"); GroupPaint = 2; return; }
            if (E == AptNext) { Sound("core_a_button"); GroupPaint = 1; return; }
            if (E == AptNextReleased || E == AptOptionReleased) { GroupPaint = 0; return; }
            if (E == AptSelect) { OpenProperties(); return; }
            if (E != AptMisc || Group.empty()) return;
            // X: carry the selection (LB held: a copy of it, 82866848)
            std::vector<int> Members = Group;
            if (bLbHeld)
            {
                std::vector<int> Copies;
                for (int H : Members) if (const int C = Duplicate(H)) Copies.push_back(C);
                if (Copies.empty()) return;
                GroupClear();
                DropperEditor::SpawnCursor();
                PickPos = FVector{};
                bPickedUp = false;
                DropperEditor::HoldGroup(Copies[0], std::vector<int>(Copies.begin() + 1, Copies.end()), false);
                SetStateTo(3);
                return;
            }
            HistoryPush();   // 825B1968
            GroupClear();
            Sound("sp_object_pickup");
            if (const DropperWorld::Placed* P = DropperWorld::Get(Members[0])) { PickPos = P->Position; PickRot = P->Rotation; }
            bPickedUp = true;
            DropperEditor::HoldGroup(Members[0], std::vector<int>(Members.begin() + 1, Members.end()), true);
            SetStateTo(3);
        }

        // 825B00B8: while A / Y is held in group select, the object under the cursor joins / leaves.
        void GroupPaintStep()
        {
            if (State != 4 || !bGroupMode) return;
            const int H = DropperEditor::Hovered();
            if (GroupPaint == 1 && H && std::find(Group.begin(), Group.end(), H) == Group.end())
            {
                Group.push_back(H);   // 82854958 (the selection has no size limit here: no object limits)
            }
            else if (GroupPaint == 2 && H)
            {
                auto It = std::find(Group.begin(), Group.end(), H);
                if (It != Group.end()) { Group.erase(It); GroupHighlight(H, false); }   // 82854C78
            }
            for (int M : Group) GroupHighlight(M, true);
        }

        void HandleEvent(int E)
        {
            switch (State)
            {
            case 1: FreeCamEvent(E); break;
            case 2: CatalogEvent(E); break;
            case 3: HoldingEvent(E); break;
            case 4: GroupEvent(E); break;
            default: break;
            }
        }

        // ---------------------------------------------------------------- properties menu
        // Skate 3's ObjectQuickMenu (FE screen 0x34, vtable 0x822fc764: open 825b5b10, activate
        // 825b5f60, close 825b6048, events 825b7af0, OnMenuSelect 825b7d28, OnMenuHilight 825b7e90,
        // OnMenuGetItemEnabled 825b7dd0) with its own movie objectquickmenu.apt.

        // FEObjectQuickMenuScreenMode_* (the movie's SetState argument), table 0x82FD0718.
        enum QMsg : int
        {
            EnableColor, EnableBranding, EnableStyle, EnableOptions, EnableCopy, EnablePaste, EnableUndo, EnableRedo,
            ActivateOptionsMenu, ActivateQuickMenu, ActivateColorMenu, ActivateStyleMenu, ActivateBrandingMenu,
            RefreshSnap, RefreshCollision, RefreshInvertYAxis, RefreshInvertXAxis, RefreshCursorSpeed,
            EnableSnap, EnableCollision, EnableInvertYAxis, EnableInvertXAxis, EnableCursor, RefreshCursor,
            RefreshColourMenuButtonCallouts, RefreshPropertiesMenuButtonCallouts, QMsgCount
        };
        const char* const QMsgNames[QMsgCount] = {
            "EnableColor", "EnableBranding", "EnableStyle", "EnableOptions", "EnableCopy", "EnablePaste", "EnableUndo",
            "EnableRedo", "ActivateOptionsMenu", "ActivateQuickMenu", "ActivateColorMenu", "ActivateStyleMenu",
            "ActivateBrandingMenu", "RefreshSnap", "RefreshCollision", "RefreshInvertYAxis", "RefreshInvertXAxis",
            "RefreshCursorSpeed", "EnableSnap", "EnableCollision", "EnableInvertYAxis", "EnableInvertXAxis", "EnableCursor",
            "RefreshCursor", "RefreshColourMenuButtonCallouts", "RefreshPropertiesMenuButtonCallouts" };

        std::unique_ptr<Apt::Player> QMovie;
        Value QScreen;
        bool bQOpen = false;
        int QOutroMs = 0;               // closing: the movie plays its outro (frames 20..29) over the dropper
        int QPrevState = 1;             // dropper +0x210
        int QItem = 0;                  // +0x3c: 0 colour 1 branding 2 style 3 options 4 copy 5 paste 6 undo 7 redo
        int QOption = 0;                // +0x40: 0 snap 1 collision 2 invert Y 3 invert X 4 cursor speed
        int QSub = 0;                   // +0x70: 0 main 1 options 2 colour 3 branding 4 style
        int QColour = 0, QColourSaved = 0;   // +0x74 / +0x78
        int QStyle = 0, QStyleSaved = 0;     // +0x88 / +0x8c
        int QBrand = 0, QBrandSaved = 0;     // +0x94 / +0x98
        int Brand = 0;                  // DAT_8300b290 + 0xadc: the park's brand set (ID_SKATEPARK_BRAND_*)
        int QInputBlockMs = 0;          // vtable +0x38(100): input ignored just after opening
        bool bCatalogProps = false;     // editor +0x8a0: opened from the catalog (the next object's properties)
        std::vector<int> QSel;          // 82866ff8: the selection when the menu opened (editor +0x554, else +0x550)
        std::vector<std::pair<int, std::string>> QRevert;   // 825b6bb0: the pieces before a property change
        std::vector<DropperWorld::Placed> Clipboard;        // dropper +0x190 (8285f860)
        std::vector<std::array<int, 3>> Tints;              // skatepark_presentation/tint, 0..255 (825b6ca0)
        // SwatchRenderer_SetColour: the colour per movie and image index
        std::map<std::pair<const Apt::Player*, int>, std::array<float, 3>> Swatches;
        void ForgetSwatches(const Apt::Player* P)
        {
            for (auto It = Swatches.begin(); It != Swatches.end();) It = It->first.first == P ? Swatches.erase(It) : std::next(It);
        }

        // sk8popup (82cdf8e8): the "apply to all" warnings with Yes / No.
        std::unique_ptr<Apt::Player> PMovie;
        Value PScreen;
        std::string PopupText;
        std::vector<std::string> PopupOptions;
        std::function<void(int)> PopupDone;

        void QSend(int M)
        {
            if (QMovie && QScreen.t == Value::Obj) QMovie->CallMethod(QScreen, "SetState", { Value::Number(M) });
        }

        bool Holding() { return DropperEditor::Held() != 0; }   // editor +0x550

        std::vector<int> LiveSel()
        {
            std::vector<int> S;
            for (int H : QSel) if (DropperWorld::Get(H)) S.push_back(H);
            return S;
        }

        void LoadTints()
        {
            if (!Tints.empty()) return;
            std::ifstream In(Root + "\\dropper\\tints.txt");
            std::string Line;
            while (std::getline(In, Line))
            {
                if (Line.empty() || Line[0] == '#') continue;
                float R = 0, G = 0, B = 0;
                if (sscanf_s(Line.c_str(), "%f %f %f", &R, &G, &B) == 3)
                    Tints.push_back({ int(R * 255.f), int(G * 255.f), int(B * 255.f) });
            }
            if (Tints.size() > 64) Tints.resize(64);   // 825b6c18: at most 0x40
        }
        int NumColours() { LoadTints(); return int(Tints.size()); }

        // The sibling piece with another style's suffix ("..._downtown" -> "..._industrial"), or "".
        std::string StyledId(const std::string& Id, int St)
        {
            const DropperData::CatalogEntry* E = DropperWorld::Library().Find(Id);
            if (!E || StyleOf(E->Name) < 0 || St < 0 || St > 2) return {};
            const std::string Want = E->Name.substr(0, E->Name.rfind('_') + 1) + Styles[St];
            for (const auto& C : DropperWorld::Library().Catalog())
                if (!Stricmp(C.Name, Want)) return C.Id;
            return {};
        }
        // 825b87b0: a piece of the selection has a style.
        bool SelectionHasStyle()
        {
            for (int H : LiveSel())
                if (const DropperWorld::Placed* P = DropperWorld::Get(H))
                    if (const DropperData::CatalogEntry* E = DropperWorld::Library().Find(P->Id))
                        if (StyleOf(E->Name) >= 0) return true;
            return false;
        }
        // 825b8920: a piece of the selection takes a colour (IsColorizable) - none of the shipped
        // pieces do.
        bool SelectionColourizable() { return false; }

        // 825b6bb0: what the pieces are before a colour / branding / style change.
        void SaveRevert()
        {
            QRevert.clear();
            for (const DropperWorld::Placed& P : DropperWorld::All()) QRevert.push_back({ P.Handle, P.Id });
            QBrandSaved = QBrand;
        }
        // 825b7a90 / 82852450: the change is undone.
        void Revert()
        {
            for (const auto& R : QRevert)
                if (const DropperWorld::Placed* P = DropperWorld::Get(R.first))
                    if (P->Id != R.second) DropperWorld::Replace(CurWorld, R.first, R.second);
        }

        // 825b4610: the catalog takes the style (its list is rebuilt, the cursor stays on the piece).
        void CatalogStyle(int St)
        {
            const std::string Sibling = StyledId(PreviewId, St);
            Db.Style = St;
            Db.Build();
            if (Sibling.empty() || !FindInCatalog(Sibling)) Recount();
            UpdatePreview();
        }
        // 82852268 + 828667d0: the selection (all: every object too) swaps to the style's pieces.
        void ApplyStyle(bool bAll, int St)
        {
            std::vector<int> Targets = LiveSel();
            if (bAll)
                for (const DropperWorld::Placed& P : DropperWorld::All())
                    if (std::find(Targets.begin(), Targets.end(), P.Handle) == Targets.end()) Targets.push_back(P.Handle);
            for (int H : Targets)
                if (const DropperWorld::Placed* P = DropperWorld::Get(H))
                {
                    const std::string To = StyledId(P->Id, St);
                    if (!To.empty()) DropperWorld::Replace(CurWorld, H, To);
                }
        }
        // 825b8728: the style takes effect (catalog: its list; else the selection).
        void TakeStyle()
        {
            if (bCatalogProps) CatalogStyle(QStyle);
            else ApplyStyle(false, QStyle);
        }

        // 825b7ec8: whether a main menu item can be picked.
        bool QGate(int Item)
        {
            switch (Item)
            {
            case 0: return bCatalogProps ? SelectionColourizable() : true;
            case 1: return true;
            case 2: return bCatalogProps ? SelectionHasStyle() : true;
            case 3: return true;
            case 4: return !bCatalogProps && !LiveSel().empty();
            case 5: return !bCatalogProps && !Clipboard.empty() && !Holding();
            case 6: return !bCatalogProps && CanUndo();
            case 7: return !bCatalogProps && CanRedo();
            default: return true;
            }
        }
        void QHighlight() { if (QItem >= 0 && QItem <= 7) QSend(QItem); }   // 825b61c0
        void QStep(int Dir)   // 825b6140 / 825b6f68 0xc: the next item that can be picked
        {
            const int From = QItem;
            for (;;)
            {
                QItem = (QItem + Dir + 8) % 8;
                if (QItem == From) break;
                if (QGate(QItem)) { Sound(Dir > 0 ? "core_nav_down" : "core_nav_up"); break; }
            }
            QHighlight();
        }
        void QBackToMain() { QSub = 0; QSend(ActivateQuickMenu); QHighlight(); }

        void CloseProperties()   // 825b6048 (editor input back, the catalog camera flag off)
        {
            bQOpen = false;
            PMovie.reset();
            PScreen = Value();
            PopupDone = nullptr;
            // the screen manager plays the outro (to frame 29: OutroComplete), then the screen goes
            if (QMovie && QScreen.t == Value::Obj)
            {
                QMovie->CallMethod(QMovie->RootObject(), "play", {});
                QOutroMs = 1000;
            }
            else QMovie.reset();
        }
        void QOutroDone()
        {
            QOutroMs = 0;
            if (bQOpen) return;
            ForgetSwatches(QMovie.get());
            QMovie.reset();
            QScreen = Value();
        }

        // 828628b0 via 82866848 (paste): the clipboard's objects carried at the cursor, laid out as copied.
        void PasteClipboard()
        {
            if (Clipboard.empty()) return;
            const FVector At = DropperEditor::SpawnCursor();
            const FVector Shift{ At.X - Clipboard[0].Position.X, At.Y - Clipboard[0].Position.Y, At.Z - Clipboard[0].Position.Z };
            std::vector<int> Made;
            for (const DropperWorld::Placed& P : Clipboard)
                if (const int H = DropperWorld::Add(CurWorld, P.Id, FVector{ P.Position.X + Shift.X, P.Position.Y + Shift.Y, P.Position.Z + Shift.Z }, P.Rotation, true)) Made.push_back(H);
            if (Made.empty()) { SetStateTo(1); return; }
            PickPos = FVector{};
            bPickedUp = false;
            DropperEditor::HoldGroup(Made[0], std::vector<int>(Made.begin() + 1, Made.end()), false);
            SetStateTo(3);
        }

        void ClosePopup()
        {
            PMovie.reset();
            PScreen = Value();
        }
        bool LoadPopup(std::string& Error);
        void OpenPopup(const std::string& Text, std::function<void(int)> Done)
        {
            PopupText = Text;
            PopupOptions = { "ID_COMMON_YES", "ID_COMMON_NO" };
            PopupDone = std::move(Done);
            std::string Error;
            if (!LoadPopup(Error)) { Log("[dropper] popup: " + Error); auto D = PopupDone; PopupDone = nullptr; if (D) D(1); }
        }
        int PopupChoice = -1;   // picked in the popup movie's own call: acted on after it
        void PopupChoose(int Ix) { PopupChoice = Ix; }
        void PopupFinish()
        {
            if (PopupChoice < 0) return;
            const int Ix = PopupChoice;
            PopupChoice = -1;
            auto D = PopupDone;
            PopupDone = nullptr;
            ClosePopup();
            if (D) D(Ix);
        }

        // 825b6f68: the main list.
        void QMainEvent(int E)
        {
            using namespace DropperInput;
            if (E == AptPrevious)
            {
                Sound("core_b_button");
                const int Back = QPrevState;
                CloseProperties();
                SetStateTo(Back);
                return;
            }
            if (E == AptUp) { QStep(-1); return; }
            if (E == AptDown) { QStep(1); return; }
            if (E != AptNext || !QGate(QItem)) return;
            switch (QItem)
            {
            case 3:   // options
                Sound("core_a_button");
                QSub = 1;
                QSend(ActivateOptionsMenu);
                QSend(EnableSnap + QOption);   // 825b62f8
                return;
            case 0:   // colour
                Sound("core_a_button");
                if (!Holding()) HistoryPush();
                QSub = 2;
                SaveRevert();
                QColourSaved = QColour;
                QSend(ActivateColorMenu);
                QSend(RefreshCursor);
                return;
            case 1:   // branding
                Sound("core_a_button");
                if (!Holding()) HistoryPush();
                QSub = 3;
                SaveRevert();
                QSend(ActivateBrandingMenu);
                return;
            case 2:   // style
                Sound("core_a_button");
                if (!Holding()) HistoryPush();
                QSub = 4;
                SaveRevert();
                QStyleSaved = QStyle;
                QSend(ActivateStyleMenu);
                return;
            case 6:   // undo
            case 7:   // redo
            {
                Sound("core_a_button");
                const bool bKeep = Holding();
                GroupClear();
                // 82866988: whatever is carried goes, the cursor back to the eye
                const std::vector<int> Others = DropperEditor::HeldGroup();
                if (const int H = DropperEditor::Release())
                {
                    DropperWorld::Remove(H);
                    for (int O : Others) DropperWorld::Remove(O);
                    if (H == Preview) { Preview = 0; PreviewId.clear(); }
                }
                DropperEditor::CursorToCamera();
                CloseProperties();
                bPickedUp = false;
                if (QItem == 6) Undo(bKeep);
                else Redo();
                SetStateTo(1);   // state 8 until the park is loaded, then the free camera
                return;
            }
            case 4:   // copy (8285f860): the selection's objects as they are
            {
                Clipboard.clear();
                for (int H : LiveSel()) if (const DropperWorld::Placed* P = DropperWorld::Get(H)) Clipboard.push_back(*P);
                const int Back = QPrevState;
                CloseProperties();
                SetStateTo(Back);
                return;
            }
            case 5:   // paste (8285f930; no object budget here): state 7, then carried at the cursor
                CloseProperties();
                PasteClipboard();
                return;
            default:
                return;
            }
        }

        // 825b63c0: options.
        void QOptionsEvent(int E)
        {
            using namespace DropperInput;
            if (E == AptPrevious) { Sound("core_b_button"); QBackToMain(); return; }
            if (E == AptUp)
            {
                const int Old = QOption--;
                if (Old == 2) QOption--;   // collision (row 1) is hidden: IsCollisionUnlocked is false
                if (QOption < 0) QOption = 4;
                Sound("core_nav_up");
                QSend(EnableSnap + QOption);
                return;
            }
            if (E == AptDown)
            {
                const int Old = QOption++;
                if (Old == 0) QOption++;
                if (QOption == 5) QOption = 0;
                Sound("core_nav_down");
                QSend(EnableSnap + QOption);
                return;
            }
            if (E != AptLeft && E != AptRight) return;
            Sound("core_nav_left");
            DropperEditor::Options O = DropperEditor::GetOptions();
            switch (QOption)
            {
            case 0: O.Snap = !O.Snap; DropperEditor::SetOptions(O); QSend(RefreshSnap); return;
            case 1: O.Collision = !O.Collision; DropperEditor::SetOptions(O); QSend(RefreshCollision); return;
            case 2: O.InvertY = !O.InvertY; DropperEditor::SetOptions(O); QSend(RefreshInvertYAxis); return;
            case 3: O.InvertX = !O.InvertX; DropperEditor::SetOptions(O); QSend(RefreshInvertXAxis); return;
            case 4:
                O.CursorSpeed += E == AptLeft ? -0.1f : 0.1f;
                O.CursorSpeed = std::min(1.5f, std::max(0.5f, O.CursorSpeed));
                DropperEditor::SetOptions(O);
                QSend(RefreshCursorSpeed);
                return;
            default: return;
            }
        }

        // 825b6700: the colour grid (8 swatches a row).
        void QColourEvent(int E)
        {
            using namespace DropperInput;
            const int N = std::max(1, NumColours());
            switch (E)
            {
            case AptNext:
                // needs a colourizable selection (none of the shipped pieces)
                if ((DropperEditor::Hovered() || Holding()) && SelectionColourizable())
                {
                    Sound("core_a_button");
                    SaveRevert();
                    QBackToMain();
                }
                return;
            case AptPrevious:
                if (!Holding()) HistoryDropLast();
                Sound("core_b_button");
                Revert();
                QColour = QColourSaved;
                QBackToMain();
                return;
            case AptMisc:   // X: every object takes the colour, after a Yes
                Sound("core_x_button");
                SaveRevert();
                OpenPopup("ID_SKATEPARK_APPLY_ALL_COLOUR", [](int Ix)   // 825b83c8 -> 825b8568
                {
                    if (Ix == 0) QBackToMain();
                    else Revert();
                });
                return;
            case AptUp:
                if (N > 8) { QColour -= 8; if (QColour < 0) QColour += N; }
                Sound("core_nav_up");
                break;
            case AptDown:
                if (N > 8) { QColour += 8; if (QColour >= N) QColour -= N; }
                Sound("core_nav_down");
                break;
            case AptLeft:
                if (--QColour < 0) QColour += N;
                Sound("core_nav_left");
                break;
            case AptRight:
                if (++QColour >= N) QColour = 0;
                Sound("core_nav_right");
                break;
            default:
                return;
            }
            QSend(RefreshCursor);   // the selection shows the colour (825b6da8: colourizable pieces only)
        }

        // 825b7af0 sub states 3 (branding) and 4 (style); their lists are the movie's menu_picker
        // (OnMenuHilight / OnMenuSelect).
        void QListEvent(int E)
        {
            using namespace DropperInput;
            if (QSub == 3)
            {
                if (E == AptNext)
                {
                    Sound("core_a_button");
                    Brand = QBrand;
                    QBackToMain();
                }
                else if (E == AptPrevious)
                {
                    if (!Holding()) HistoryDropLast();
                    if (QBrand != QBrandSaved) { QBrand = QBrandSaved; Brand = QBrandSaved; }
                    Sound("core_b_button");
                    Revert();
                    QBackToMain();
                }
                return;
            }
            if (E == AptPrevious)
            {
                if (!Holding()) HistoryDropLast();
                Sound("core_b_button");
                Revert();
                QStyle = QStyleSaved;
                if (bCatalogProps) CatalogStyle(QStyle);
                QBackToMain();
                return;
            }
            if (E == AptMisc)   // X: every object takes the style, after a Yes (825b86a0(.., 0) -> 825b8498)
            {
                ApplyStyle(true, QStyle);
                OpenPopup("ID_SKATEPARK_APPLY_ALL_STYLE", [](int Ix)   // 825b8608
                {
                    if (Ix == 0) QBackToMain();
                    else Revert();
                });
            }
        }

        // Game.OnMenuHilight (825b7e90): branding / style previews the hilighted entry.
        void QHilight(int Value)
        {
            if (QSub == 3) { QBrand = Value; Brand = Value; }
            else if (QSub == 4) { QStyle = Value; TakeStyle(); }
        }
        // Game.OnMenuSelect (825b7d28): a style is picked.
        void QSelect(int Value)
        {
            if (QSub != 4 || !SelectionHasStyle()) return;
            Sound("core_a_button");
            QStyle = Value;
            TakeStyle();
            QBackToMain();
        }

        void QuickEvent(int E)
        {
            if (QInputBlockMs > 0) return;
            switch (QSub)
            {
            case 0: QMainEvent(E); break;
            case 1: QOptionsEvent(E); break;
            case 2: QColourEvent(E); break;
            default: QListEvent(E); break;
            }
            // the movie's own input (its style / branding list, while mbAllowMenuPickerInput)
            if (bQOpen && QMovie && QScreen.t == Value::Obj && !PMovie)
                QMovie->CallMethod(QScreen, "UpdateInput", { Value::Number(E), Value::Number(0) });
        }

        void PopupEvent(int E)
        {
            if (PMovie && PScreen.t == Value::Obj)
                PMovie->CallMethod(PScreen, "UpdateInput", { Value::Number(E), Value::Number(0) });
        }

        bool LoadQuickMenu(std::string& Error);

        // 825b1b90 -> 825b5b10 / 825b5f60: open the properties menu over the dropper.
        void OpenProperties()
        {
            if (bQOpen) return;
            QOutroDone();
            bCatalogProps = State == 2;
            // 82866ff8: the selection (hovered object, else the carried one and its group; the
            // catalog: its object at the cursor)
            QSel.clear();
            if (bCatalogProps) { if (Preview) QSel.push_back(Preview); }
            else if (State == 4 && !Group.empty()) QSel = Group;
            else if (const int H = DropperEditor::Hovered()) QSel.push_back(H);
            else if (const int H = DropperEditor::Held())
            {
                QSel.push_back(H);
                for (int O : DropperEditor::HeldGroup()) QSel.push_back(O);
            }
            std::string Error;
            if (!LoadQuickMenu(Error)) { Log("[dropper] cannot open the properties menu: " + Error); return; }
            Sound("sp_properties");
            bQOpen = true;
            QPrevState = State;
            State = 6;
            if (bDuplicate) { bDuplicate = false; Send(RefreshDuplicate); }
            if (!bCatalogProps) EditorFor(0);
            QStyle = Db.Style;   // 82852af0(catalog +0xf8)
            QSub = 0;
            QItem = 0;
            QOption = 0;
            QBrand = Brand;
            QInputBlockMs = 100;
            if (!QGate(QItem)) QStep(1);
            else QHighlight();
        }

        // ---------------------------------------------------------------- movie

        Value Arr(const std::vector<Value>& V) { return Value::Object(Movie->NewArray(V)); }

        void InstallNatives()
        {
            Apt::Player& P = *Movie;
            Apt::ObjRef G = P.Global();
            auto N = [&](Apt::Native F) { return P.NewNative(std::move(F)); };
            auto Bool = [&](std::function<bool()> F) { return N([F](Apt::Player&, const Value&, Args&) { return Value::Boolean(F()); }); };
            auto Num = [&](std::function<double()> F) { return N([F](Apt::Player&, const Value&, Args&) { return Value::Number(F()); }); };

            for (int i = 0; i < MsgCount; i++) G->props[std::string("FEObjectDropperScreenMode_") + MsgNames[i]] = Value::Number(i);
            G->props["Screen_EdgeOffset"] = Value::Number(0);
            G->props["FE_SOUND_CORE_NAV_DOWN"] = Value::String("core_nav_down");
            G->props["FE_SOUND_CORE_NAV_UP"] = Value::String("core_nav_up");
            G->props["FE_SOUND_CORE_A_BUTTON"] = Value::String("core_a_button");

            Apt::ObjRef Park = P.NewObject();
            auto& K = Park->props;
            K["ItemSelectionReady"] = N([](Apt::Player&, const Value&, Args&) { bItemReady = true; return Value(); });
            K["AreWeInSkatePark"] = Bool([] { return false; });
            K["GetSuperCategories"] = N([](Apt::Player&, const Value&, Args&)
            {
                std::vector<Value> Out;
                for (auto& S : Db.Supers) Out.push_back(Value::String(LocalizeOr("ID_SKATEPARK_CATEGORY_" + S.Halid, "ID_STRING_MISSING")));
                return Arr(Out);
            });
            K["GetSubCategories"] = N([](Apt::Player&, const Value&, Args&)
            {
                std::vector<Value> Out;
                if (const Super* S = CurSuper())
                    for (auto& B : S->Subs)
                    {
                        char Buf[256];
                        snprintf(Buf, sizeof(Buf), "%s (%d)", LocalizeOr("ID_SKATEPARK_CATEGORY_" + B.Halid, "ID_STRING_MISSING").c_str(), int(B.Items.size()));
                        Out.push_back(Value::String(Buf));
                    }
                return Arr(Out);
            });
            K["GetNumSubCategories"] = Num([] { return double(NumSubs()); });
            K["GetSubCategoryName"] = N([](Apt::Player&, const Value&, Args& A)
            {
                const Super* S = CurSuper();
                const int Ix = A.empty() ? SubIx : int(A[0].ToNum());
                if (!S || Ix < 0 || Ix >= int(S->Subs.size())) return Value::String("ID_STRING_MISSING");
                return Value::String(LocalizeOr("ID_SKATEPARK_CATEGORY_" + S->Subs[size_t(Ix)].Halid, "ID_STRING_MISSING"));
            });
            K["GetSubCategoryDescription"] = N([](Apt::Player&, const Value&, Args& A)
            {
                const Super* S = CurSuper();
                const int Ix = A.empty() ? SubIx : int(A[0].ToNum());
                if (!S || Ix < 0 || Ix >= int(S->Subs.size())) return Value::String("ID_STRING_MISSING");
                return Value::String(LocalizeOr("ID_SKATEPARK_CATEGORY_" + S->Subs[size_t(Ix)].Halid + "_DESCRIPTION", "ID_STRING_MISSING"));
            });
            K["GetTotalItemsUnlockedForSubCategory"] = N([](Apt::Player&, const Value&, Args& A)
            {
                const Super* S = CurSuper();
                const int Ix = A.empty() ? SubIx : int(A[0].ToNum());
                return Value::Number(S && Ix >= 0 && Ix < int(S->Subs.size()) ? double(S->Subs[size_t(Ix)].Items.size()) : 0.0);
            });
            // 8259c8d8: item names (8285b358 props: after the last '_'; 8285b248 terrain: from 3 to the next '_').
            K["GetItems"] = N([](Apt::Player&, const Value&, Args&)
            {
                std::vector<Value> Out;
                const Super* S = CurSuper();
                if (const Sub* B = CurSub())
                    for (const auto* E : B->Items)
                    {
                        const std::string& Nm = E->Name;
                        std::string Key;
                        if (Nm.find('_') == 2)
                        {
                            if (S && S->Terrain)
                            {
                                const size_t U = Nm.find('_', 3);
                                Key = Nm.substr(3, U == std::string::npos ? std::string::npos : U - 3);
                            }
                            else Key = Nm.substr(Nm.rfind('_') + 1);
                            std::string Up = "ID_SKATEPARK_OBJECT_" + Key;
                            Out.push_back(Value::String(LocalizeOr(Up, Up)));
                        }
                        else Out.push_back(Value::String("#" + Nm));
                    }
                return Arr(Out);
            });
            K["GetItemsUnlocked"] = N([](Apt::Player&, const Value&, Args&)
            {
                return Arr(std::vector<Value>(size_t(NumItems()), Value::Boolean(true)));
            });
            K["GetItemsNewlyUnlocked"] = N([](Apt::Player&, const Value&, Args&)
            {
                return Arr(std::vector<Value>(size_t(NumItems()), Value::Boolean(false)));
            });
            K["GetCurrentSuperCategoryIndex"] = Num([] { return double(SuperIx); });
            K["GetCurrentSubCategoryIndex"] = Num([] { return double(SubIx); });
            K["GetCurrentItemIndex"] = Num([] { return double(ItemIx); });
            // 8259df48: "Width / Height / Depth" of the carried object (metres -> the {n:distance.1} format: feet).
            K["GetCurrentItemSizeInfo"] = N([](Apt::Player&, const Value&, Args&)
            {
                const int H = DropperEditor::Held() ? DropperEditor::Held() : Preview;
                const DropperWorld::Placed* Pl = DropperWorld::Get(H);
                if (!Pl) return Value::String("");
                auto Obj = DropperWorld::Library().Get(Pl->Id);
                if (!Obj) return Value::String("");
                const double Dim[3] = { Obj->Max[0] - Obj->Min[0], Obj->Max[1] - Obj->Min[1], Obj->Max[2] - Obj->Min[2] };
                std::string Fmt = Localize("ID_SKATEPARK_ITEM_SIZE"), Out;
                for (size_t i = 0; i < Fmt.size();)
                {
                    if (Fmt[i] == '{' && i + 1 < Fmt.size() && isdigit((unsigned char)Fmt[i + 1]))
                    {
                        const size_t End = Fmt.find('}', i);
                        const int Ix = Fmt[i + 1] - '0';
                        char Buf[32];
                        snprintf(Buf, sizeof(Buf), "%.1f ft", (Ix >= 0 && Ix < 3 ? Dim[Ix] : 0.0) * 3.280840);
                        Out += Buf;
                        i = End == std::string::npos ? Fmt.size() : End + 1;
                        continue;
                    }
                    Out += Fmt[i++];
                }
                return Value::String(Out);
            });
            K["IsFengShuiShowingWarning"] = Bool([] { return false; });
            K["IsUIEnabled"] = Bool([] { return bUIEnabled; });
            K["IsCursorVisible"] = Bool([] { return bCursorVisible; });
            K["IsSpawnerPresent"] = Bool([] { return false; });
            K["IsCurrentItemTerrain"] = Bool([] { const Super* S = CurSuper(); return S && S->Terrain; });
            K["IsWorldSpawner"] = Bool([] { return false; });
            K["GetUnlockString"] = N([](Apt::Player&, const Value&, Args&) { return Value::String(""); });
            K["GetSize"] = Num([]
            {
                const DropperData::CatalogEntry* E = CurItem();
                return double(E ? SizeOf(E->Name) : 0);
            });
            K["GetStyle"] = N([](Apt::Player&, const Value&, Args&)
            {
                const DropperData::CatalogEntry* E = CurItem();
                const int St = E ? StyleOf(E->Name) : -1;
                return Value::String(St == 0 ? "downtown" : St == 1 ? "industrial" : St == 2 ? "uni" : "invalid");
            });
            // 8259dd20: the properties menu's colour (skatepark_presentation/tint [+0x74]) as r, g, b 0..255
            K["GetColour"] = N([](Apt::Player& Pl, const Value&, Args&)
            {
                LoadTints();
                std::vector<Value> Out;
                if (QColour >= 0 && QColour < int(Tints.size()))
                    for (int c = 0; c < 3; c++) Out.push_back(Value::Number(Tints[size_t(QColour)][size_t(c)]));
                else Out = { Value::Number(0), Value::Number(0), Value::Number(0) };
                return Value::Object(Pl.NewArray(Out));
            });
            K["IsItemSelected"] = Bool([] { return DropperEditor::Held() != 0; });
            K["IsFineTuneModeEnabled"] = Bool([] { return bFineTune; });
            K["IsDuplicateModeEnabled"] = Bool([] { return bDuplicate; });
            K["IsDuplicateModeAllowed"] = Bool([] { return true; });
            K["IsSelectingSubCategory"] = Bool([] { return bSelectingSub; });
            K["IsObjectDropperEnabled"] = Bool([] { return true; });
            G->props["SkatePark"] = Value::Object(Park);

            Apt::ObjRef Game = P.NewObject();
            auto Nop = N([](Apt::Player&, const Value&, Args&) { return Value(); });
            Game->props["OnMenuHilight"] = Nop;
            Game->props["OnMenuSelect"] = Nop;
            Game->props["OnMenuGetItemEnabled"] = Bool([] { return true; });
            int NextImage = 0;
            Game->props["ImageRenderer_ReserveNextFreeIndex"] = N([NextImage](Apt::Player&, const Value&, Args&) mutable { return Value::Number(NextImage++); });
            Game->props["SwatchRenderer_ReserveIndex"] = N([NextImage](Apt::Player&, const Value&, Args&) mutable { return Value::Number(NextImage++); });
            for (const char* Name : { "ImageRenderer_FreeImageIndex", "ImageRenderer_Load", "ImageRenderer_LoadAsset", "ImageRenderer_LoadAssetByName",
                                      "ImageRenderer_MemcardLoad", "ImageRenderer_OnlineLoad", "ImageRenderer_Unload", "EnableDimmer", "HideDummyDisplay" })
                Game->props[Name] = Nop;
            Game->props["SwatchRenderer_SetColour"] = N([](Apt::Player& Pl, const Value&, Args& A)
            {
                if (A.size() >= 4)
                    Swatches[{ &Pl, int(A[0].ToNum()) }] = { float(A[1].ToNum() / 255.0), float(A[2].ToNum() / 255.0), float(A[3].ToNum() / 255.0) };
                return Value();
            });
            Game->props["SwatchRenderer_FreeIndex"] = N([](Apt::Player& Pl, const Value&, Args& A)
            {
                if (!A.empty()) Swatches.erase({ &Pl, int(A[0].ToNum()) });
                return Value();
            });
            G->props["Game"] = Value::Object(Game);

            Apt::ObjRef AudioObj = P.NewObject();
            AudioObj->props["PlaySound"] = N([](Apt::Player&, const Value&, Args& A)
            {
                if (!A.empty() && A[0].t == Value::Str)
                {
                    static std::unordered_map<std::string, std::string> Names;   // stable storage for the cue names
                    auto It = Names.emplace(A[0].s, A[0].s).first;
                    Sound(It->second.c_str());
                }
                return Value();
            });
            G->props["Audio"] = Value::Object(AudioObj);

            Apt::ObjRef Screens = P.NewObject();
            Screens->props["OnLoaded"] = Nop;
            Screens->props["IntroComplete"] = Nop;
            Screens->props["OutroComplete"] = Nop;
            G->props["ScreenManager"] = Value::Object(Screens);

        }

        bool LoadMovie(std::string& Error)
        {
            ForgetSwatches(Movie.get());
            Movie = std::make_unique<Apt::Player>();
            Movie->LogLine = [](const std::string& S) { Log("[dropper apt] " + S); };
            Movie->Localize = [](const std::string& Key) { return Localize(Key); };
            Movie->InlineIcon = [](const std::string& Name)
            {
                const int Ix = IconFor(Name);
                return Ix < 0 || !Movie ? -1 : int(Movie->Textures().size()) + Ix;
            };
            const std::string Controls = Root + "\\extracted\\dropper_controls";
            for (const char* M : { "menu_picker", "menu_part_text_hilite", "menu_part_glow", "scrollbar", "tabs", "swatch_renderer", "button_item2" })
                Movie->ImportDirs[M] = { Controls, Root + "\\hud\\" + M };
            const std::string Font = Root + "\\font\\";
            const std::unordered_map<std::string, std::string> Fonts = {
                { "Futura Shadow", Font + "futurashadow.txt" },
                { "FuturaOuterGlow", Font + "futuraglow.txt" },
                { "Futura Glow", Font + "futuraglow.txt" },
                { "Futura Std Medium", Font + "futuraheavy.txt" },
                { "Futura Overprint", Font + "futuraoverprint.txt" },
            };
            if (!Movie->Load(Root + "\\extracted\\dropper_fe", "objectdropper", Root + "\\hud\\objectdropper", Fonts))
            {
                Error = Movie->Error();
                Movie.reset();
                return false;
            }
            InstallNatives();
            Movie->Advance(0);   // frame 0: "screen = new objectdropper(this)"
            Screen = Movie->Get(Movie->RootObject(), "screen");
            if (Screen.t != Value::Obj) { Error = "objectdropper screen object not found"; Movie.reset(); return false; }
            return true;
        }

        void Close()
        {
            CloseProperties();
            bOpen = false;
            bClosed = true;
            DropperEditor::Exit();
            ForgetSwatches(Movie.get());
            Movie.reset();
            Screen = Value();
        }

        // ---- the properties menu's and the popup's movies ----

        const std::unordered_map<std::string, std::string>& FontMap()
        {
            static std::unordered_map<std::string, std::string> Fonts;
            if (Fonts.empty())
            {
                const std::string Font = Root + "\\font\\";
                Fonts = {
                    { "Futura Shadow", Font + "futurashadow.txt" },
                    { "FuturaOuterGlow", Font + "futuraglow.txt" },
                    { "Futura Glow", Font + "futuraglow.txt" },
                    { "Futura Std Medium", Font + "futuraheavy.txt" },
                    { "Futura Std Heavy", Font + "futuraheavy.txt" },
                    { "Futura Heavy", Font + "futuraheavy.txt" },
                    { "Futura Overprint", Font + "futuraoverprint.txt" },
                    // button_menu's labels name "Marker_streak", which Skate 3 does not ship (miscboot
                    // has only the four Futura bmpFonts): the front end's text font stands in
                    { "Marker_streak", Font + "futuraheavy.txt" },
                };
            }
            return Fonts;
        }

        // A front-end movie with source/controls imports (quickmenu_controls first, then dropper_controls).
        std::unique_ptr<Apt::Player> NewMovie(const std::string& Name, const char* Tag)
        {
            auto P = std::make_unique<Apt::Player>();
            const std::string T = Tag;
            P->LogLine = [T](const std::string& S) { Log("[" + T + " apt] " + S); };
            P->Localize = [](const std::string& Key) { return Localize(Key); };
            Apt::Player* Raw = P.get();
            P->InlineIcon = [Raw](const std::string& N)
            {
                const int Ix = IconFor(N);
                return Ix < 0 ? -1 : int(Raw->Textures().size()) + Ix;
            };
            const std::string Quick = Root + "\\extracted\\quickmenu_controls", Controls = Root + "\\extracted\\dropper_controls";
            for (const char* M : { "panel", "highlight_selector", "highlight_slider", "highlight_option", "highlight_color_grid", "button_menu", "sk8popup" })
                P->ImportDirs[M] = { Quick, Root + "\\hud\\" + M };
            for (const char* M : { "menu_picker", "menu_part_text_hilite", "menu_part_glow", "scrollbar", "tabs", "swatch_renderer", "button_item2", "image_renderer" })
                P->ImportDirs[M] = { Controls, Root + "\\hud\\" + M };
            (void)Name;
            return P;
        }

        // Globals every front-end movie reads.
        void InstallFrontEnd(Apt::Player& P, Apt::ObjRef& Game)
        {
            Apt::ObjRef G = P.Global();
            auto N = [&](Apt::Native F) { return P.NewNative(std::move(F)); };
            auto Nop = N([](Apt::Player&, const Value&, Args&) { return Value(); });
            G->props["Screen_EdgeOffset"] = Value::Number(0);
            G->props["FE_SOUND_CORE_NAV_DOWN"] = Value::String("core_nav_down");
            G->props["FE_SOUND_CORE_NAV_UP"] = Value::String("core_nav_up");
            G->props["FE_SOUND_CORE_A_BUTTON"] = Value::String("core_a_button");
            G->props["FE_SOUND_CORE_B_BUTTON"] = Value::String("core_b_button");
            // the input key codes passed to UpdateInput (the AptEvent order)
            const char* const Keys[] = { "AptStart", "AptSelect", "AptNext", "AptPrevious", "AptOption", "AptMisc",
                                         "AptRShift1", "AptRShift2", "AptRShift3", "AptLShift1", "AptLShift2", "AptLShift3",
                                         "AptUp", "AptDown", "AptLeft", "AptRight" };
            for (int i = 0; i < int(std::size(Keys)); i++) G->props[Keys[i]] = Value::Number(i);

            Game = P.NewObject();
            int NextImage = 0;
            Game->props["ImageRenderer_ReserveNextFreeIndex"] = N([NextImage](Apt::Player&, const Value&, Args&) mutable { return Value::Number(NextImage++); });
            Game->props["SwatchRenderer_ReserveIndex"] = N([NextImage](Apt::Player&, const Value&, Args&) mutable { return Value::Number(1000 + NextImage++); });
            Game->props["SwatchRenderer_SetColour"] = N([](Apt::Player& Pl, const Value&, Args& A)
            {
                if (A.size() >= 4)
                    Swatches[{ &Pl, int(A[0].ToNum()) }] = { float(A[1].ToNum() / 255.0), float(A[2].ToNum() / 255.0), float(A[3].ToNum() / 255.0) };
                return Value();
            });
            Game->props["SwatchRenderer_FreeIndex"] = N([](Apt::Player& Pl, const Value&, Args& A)
            {
                if (!A.empty()) Swatches.erase({ &Pl, int(A[0].ToNum()) });
                return Value();
            });
            for (const char* Name : { "ImageRenderer_FreeImageIndex", "ImageRenderer_Load", "ImageRenderer_LoadAsset", "ImageRenderer_LoadAssetByName",
                                      "ImageRenderer_MemcardLoad", "ImageRenderer_OnlineLoad", "ImageRenderer_Unload", "EnableDimmer", "HideDummyDisplay" })
                Game->props[Name] = Nop;
            G->props["Game"] = Value::Object(Game);

            Apt::ObjRef AudioObj = P.NewObject();
            AudioObj->props["PlaySound"] = N([](Apt::Player&, const Value&, Args& A)
            {
                if (!A.empty() && A[0].t == Value::Str)
                {
                    static std::unordered_map<std::string, std::string> Names;
                    auto It = Names.emplace(A[0].s, A[0].s).first;
                    Sound(It->second.c_str());
                }
                return Value();
            });
            G->props["Audio"] = Value::Object(AudioObj);

            Apt::ObjRef Screens = P.NewObject();
            Screens->props["OnLoaded"] = Nop;
            Screens->props["IntroComplete"] = Nop;
            Screens->props["OutroComplete"] = Nop;
            G->props["ScreenManager"] = Value::Object(Screens);
        }

        bool LoadQuickMenu(std::string& Error)
        {
            LoadTints();
            ForgetSwatches(QMovie.get());
            QMovie = NewMovie("objectquickmenu", "properties");
            Apt::Player& P = *QMovie;
            if (!P.Load(Root + "\\extracted\\quickmenu_controls", "objectquickmenu", Root + "\\hud\\objectquickmenu", FontMap()))
            {
                Error = P.Error();
                QMovie.reset();
                return false;
            }
            Apt::ObjRef Game;
            InstallFrontEnd(P, Game);
            Apt::ObjRef G = P.Global();
            auto N = [&](Apt::Native F) { return P.NewNative(std::move(F)); };
            auto Bool = [&](std::function<bool()> F) { return N([F](Apt::Player&, const Value&, Args&) { return Value::Boolean(F()); }); };
            auto Num = [&](std::function<double()> F) { return N([F](Apt::Player&, const Value&, Args&) { return Value::Number(F()); }); };
            for (int i = 0; i < QMsgCount; i++) G->props[std::string("FEObjectQuickMenuScreenMode_") + QMsgNames[i]] = Value::Number(i);

            Game->props["ColourPicker_GetColours"] = N([](Apt::Player& Pl, const Value&, Args&)   // 825b6ca0 per swatch
            {
                std::vector<Value> Out;
                for (const auto& T : Tints) for (int c = 0; c < 3; c++) Out.push_back(Value::Number(T[size_t(c)]));
                return Value::Object(Pl.NewArray(Out));
            });
            Game->props["ColourPicker_GetColourIndex"] = Num([] { return double(QColour); });
            Game->props["OnMenuHilight"] = N([](Apt::Player&, const Value&, Args& A) { if (A.size() >= 2) QHilight(int(A[1].ToNum())); return Value(); });
            Game->props["OnMenuSelect"] = N([](Apt::Player&, const Value&, Args& A) { if (A.size() >= 2) QSelect(int(A[1].ToNum())); return Value(); });
            // 825b7dd0: branding entries 2..6 (online / user created) need online content
            Game->props["OnMenuGetItemEnabled"] = N([](Apt::Player&, const Value&, Args& A)
            {
                const int Ix = A.size() >= 2 ? int(A[1].ToNum()) : 0;
                return Value::Boolean(QSub != 3 || Ix < 2 || Ix > 6);
            });

            Apt::ObjRef Park = P.NewObject();
            auto& K = Park->props;
            const DropperEditor::Options O0 = DropperEditor::GetOptions();
            (void)O0;
            K["QuickMenu_IsQuickMenuItemEnabled"] = N([](Apt::Player&, const Value&, Args& A) { return Value::Boolean(QGate(A.empty() ? 0 : int(A[0].ToNum()))); });
            K["QuickMenu_IsOptionItemEnabled"] = Bool([] { return true; });   // 8262e4d8
            K["IsCollisionUnlocked"] = Bool([] { return false; });            // 8259dcf8
            K["GetCurrentSnapState"] = Bool([] { return DropperEditor::GetOptions().Snap; });
            K["GetCurrentCollisionState"] = Bool([] { return DropperEditor::GetOptions().Collision; });
            K["GetCurrentInvertXAxisState"] = Bool([] { return DropperEditor::GetOptions().InvertX; });
            K["GetCurrentInvertYAxisState"] = Bool([] { return DropperEditor::GetOptions().InvertY; });
            K["GetCurrentCursorSpeed"] = Num([]   // 8259d198
            {
                const int V = int((DropperEditor::GetOptions().CursorSpeed - 0.4999f) * 10.f);
                return double(V < 0 ? 0 : V > 9 ? 10 : V);
            });
            K["IsPropertiesMenuBackButtonAllowed"] = Bool([] { return true; });
            K["IsPropertiesMenuAcceptButtonAllowed"] = Bool([] { return true; });
            K["IsColorChangeApplyAllowed"] = Bool([] { return SelectionColourizable(); });
            K["IsColorChangeApplyAllAllowed"] = Bool([] { return true; });
            K["IsColorChangeBackAllowed"] = Bool([] { return true; });
            K["IsStyleChangeAllowed"] = Bool([] { return SelectionHasStyle(); });
            K["GetNumVisualTypes"] = Num([] { return 3.0; });
            K["GetCurrentVisualTypeIndex"] = Num([] { return double(QStyle); });
            K["GetVisualTypeName"] = N([](Apt::Player&, const Value&, Args& A)   // 8259d840: fe_park_itemtypes halid
            {
                static const char* const Halid[3] = { "plaza", "industrial", "university" };
                const int Ix = A.empty() ? 0 : int(A[0].ToNum());
                return Value::String(Ix >= 0 && Ix < 3 ? std::string("ID_SKATEPARK_VISUALTYPE_") + Halid[Ix] : std::string("ID_STRING_MISSING"));
            });
            K["GetNumBrands"] = Num([] { return 7.0; });
            K["GetCurrentBrand"] = Num([] { return double(Brand); });
            K["GetBrandName"] = N([](Apt::Player&, const Value&, Args& A)   // 8259d650
            {
                static const char* const Names[7] = { "ID_SKATEPARK_BRAND_OFF", "ID_SKATEPARK_BRAND_LOCAL", "ID_SKATEPARK_BRAND_ONLINE",
                    "ID_SKATEPARK_BRAND_USERCREATED1", "ID_SKATEPARK_BRAND_USERCREATED2", "ID_SKATEPARK_BRAND_USERCREATED3",
                    "ID_SKATEPARK_BRAND_USERCREATED4" };
                const int Ix = A.empty() ? 0 : int(A[0].ToNum());
                return Value::String(Ix >= 0 && Ix < 7 ? Names[Ix] : "#N/A");
            });
            G->props["SkatePark"] = Value::Object(Park);

            Value Screens = P.Get(Value::Object(G), "ScreenManager");
            if (Screens.t == Value::Obj)
                Screens.o->props["OutroComplete"] = N([](Apt::Player&, const Value&, Args&) { if (!bQOpen && QOutroMs) QOutroMs = 1; return Value(); });   // gone next update (not inside its own call)

            P.Advance(0);   // frame 0: "screen = new objectquickmenu(this)" (it stops the movie there)
            QScreen = P.Get(P.RootObject(), "screen");
            if (QScreen.t != Value::Obj) { Error = "objectquickmenu screen object not found"; QMovie.reset(); return false; }
            P.CallMethod(P.RootObject(), "play", {});   // the screen manager starts the intro (frame 19: IntroComplete)
            return true;
        }

        bool LoadPopup(std::string& Error)
        {
            PMovie = NewMovie("sk8popup", "popup");
            Apt::Player& P = *PMovie;
            if (!P.Load(Root + "\\extracted\\quickmenu_controls", "sk8popup", Root + "\\hud\\sk8popup", FontMap()))
            {
                Error = P.Error();
                PMovie.reset();
                return false;
            }
            Apt::ObjRef Game;
            InstallFrontEnd(P, Game);
            auto N = [&](Apt::Native F) { return P.NewNative(std::move(F)); };
            Game->props["OnMenuHilight"] = N([](Apt::Player&, const Value&, Args&) { return Value(); });
            Game->props["OnMenuGetItemEnabled"] = N([](Apt::Player&, const Value&, Args&) { return Value::Boolean(true); });
            Game->props["OnMenuSelect"] = N([](Apt::Player&, const Value&, Args& A)
            {
                const int Ix = A.size() >= 2 ? int(A[1].ToNum()) : 0;
                PopupChoose(Ix);
                return Value();
            });
            Apt::ObjRef Popup = P.NewObject();
            Popup->props["GetDescription"] = N([](Apt::Player&, const Value&, Args&) { return Value::String(PopupText); });
            Popup->props["GetNumOptions"] = N([](Apt::Player&, const Value&, Args&) { return Value::Number(double(PopupOptions.size())); });
            Popup->props["GetOption"] = N([](Apt::Player&, const Value&, Args& A)
            {
                const int Ix = A.empty() ? 0 : int(A[0].ToNum());
                return Value::String(Ix >= 0 && Ix < int(PopupOptions.size()) ? PopupOptions[size_t(Ix)] : std::string());
            });
            Popup->props["IsPopupCancelable"] = N([](Apt::Player&, const Value&, Args&) { return Value::Boolean(false); });
            P.Global()->props["Popup"] = Value::Object(Popup);

            P.Advance(0);
            PScreen = P.Get(P.RootObject(), "screen");
            if (PScreen.t != Value::Obj) { Error = "sk8popup screen object not found"; PMovie.reset(); return false; }
            // the screen manager shows the screen (it hides itself until then), starts its intro
            // (OnLoaded) and gives it its first state (SetState(0): Refresh)
            P.Set(P.RootObject(), "_visible", Value::Boolean(true));
            P.CallMethod(PScreen, "OnLoaded", {});
            P.CallMethod(PScreen, "SetState", { Value::Number(0) });
            return true;
        }

        // button_item2's mButtonRender: its placeholder shape is replaced by the named glyph
        // (bounds of button_item2 character 1: -16.5..15.5).
        void FindButtons(Apt::Player& Pl, Apt::Instance& I, const Apt::Mat& Parent, const Apt::Cx& ParentCx, int IconBase, std::vector<Apt::DrawCmd>& Icons,
                         std::vector<Apt::Instance*>& Hidden, std::vector<std::pair<Apt::Instance*, Apt::Cx>>& Recoloured)
        {
            if (!I.visible) return;
            const Apt::Mat Mx = Parent * I.m;
            const Apt::Cx C = ParentCx * I.cx;
            if (I.obj)
            {
                auto B = I.obj->props.find("_test");
                if (B != I.obj->props.end() && B->second.t == Value::Str)
                {
                    const int Ix = IconFor(B->second.s);
                    if (Ix >= 0 && C.mul[3] + C.add[3] > 0.004f)
                    {
                        Apt::DrawCmd D;
                        D.Texture = IconBase + Ix;
                        for (int k = 0; k < 4; k++) { D.Mul[k] = C.mul[k]; D.Add[k] = C.add[k]; }
                        const float X0 = -16.5f, Y0 = -16.5f, X1 = 15.5f, Y1 = 15.5f;
                        Apt::Vertex V[4];
                        const float Px[4] = { X0, X1, X1, X0 }, Py[4] = { Y0, Y0, Y1, Y1 }, U[4] = { 0, 1, 1, 0 }, W[4] = { 0, 0, 1, 1 };
                        for (int k = 0; k < 4; k++) { Mx.Apply(Px[k], Py[k], V[k].x, V[k].y); V[k].u = U[k]; V[k].v = W[k]; }
                        D.Verts = { V[0], V[1], V[2], V[0], V[2], V[3] };
                        Icons.push_back(std::move(D));
                    }
                    Hidden.push_back(&I);
                    return;
                }
                // swatch_renderer: its render component (named by its image index) is the colour the
                // game draws there (SwatchRenderer_SetColour)
                auto Ix = I.obj->props.find("ImageIndex");
                if (Ix != I.obj->props.end() && I.obj->props.count("bInitialized"))
                {
                    // Init renamed mRenderComponent to its image index ("" + ImageIndex)
                    const int Index = int(Ix->second.ToNum());
                    const std::string Named = std::to_string(Index);
                    Apt::Instance* Ri = nullptr;
                    for (auto& Ch : I.children) if (Ch.second->name == Named) { Ri = Ch.second.get(); break; }
                    auto Sw = Swatches.find({ &Pl, Index });
                    if (Sw != Swatches.end() && Ri)
                        {
                            Recoloured.push_back({ Ri, Ri->cx });
                            for (int k = 0; k < 3; k++) { Ri->cx.mul[k] = 0; Ri->cx.add[k] = Sw->second[size_t(k)]; }
                        }
                }
            }
            for (auto& Child : I.children) FindButtons(Pl, *Child.second, Mx, C, IconBase, Icons, Hidden, Recoloured);
        }
    }

    void SetLog(std::function<void(const std::string&)> L) { LogFn = std::move(L); }
    void SetSoundSink(std::function<void(const char*)> Play) { SoundFn = std::move(Play); }

    bool Load(const std::string& DataRoot, std::string& Error)
    {
        Root = DataRoot;
        Strings.clear();
        std::ifstream In(DataRoot + "\\strings_en.txt", std::ios::binary);
        std::string Line;
        while (std::getline(In, Line))
        {
            if (!Line.empty() && Line.back() == '\r') Line.pop_back();
            const size_t Start = Line.find_first_not_of(' ');
            const size_t Tab = Line.find('\t');
            if (Start == std::string::npos || Tab == std::string::npos || Tab < Start) continue;
            Strings[Line.substr(Start, Tab - Start)] = Line.substr(Tab + 1);
        }
        if (!DropperWorld::Load(Error)) return false;
        {
            std::string SnapError;
            if (!DropperSnap::Load(DataRoot + "\\dropper\\snaps.txt", SnapError)) Log("[dropper] snapping off: " + SnapError);
        }
        Db.Build();
        size_t Items = 0;
        for (auto& S : Db.Supers) for (auto& B : S.Subs) Items += B.Items.size();
        char Buf[128];
        snprintf(Buf, sizeof(Buf), "[dropper] catalog: %zu categories, %zu items at size %d / style %s", Db.Supers.size(), Items, Db.Size, Styles[Db.Style]);
        Log(Buf);
        // a test load of the movie (it is loaded fresh each time the dropper opens)
        if (!LoadMovie(Error)) return false;
        Movie.reset();
        Screen = Value();
        return true;
    }

    bool Open(UWorld* World, const FVector& Start, double FacingYaw)
    {
        if (bOpen) return true;
        std::string Error;
        if (!LoadMovie(Error)) { Log("[dropper] cannot open: " + Error); return false; }
        CurWorld = World;
        bOpen = true;
        bClosed = false;
        Accum = 0;
        State = 0;
        bSelectingSub = true;
        bDuplicate = bFineTune = bAPressed = bYRepeat = bPickedUp = false;
        ShiftMode = 0;
        HeldMode = 2;
        InputBlockMs = 0;
        Recount();
        DropperEditor::Enter(Start, FacingYaw);
        HistoryPush();   // 825B57E8: the park as it was when the dropper opened
        Sound("sp_object_dropper_enter");
        SetStateTo(1);
        return true;
    }

    bool IsOpen() { return bOpen; }
    bool TakeClosed() { const bool B = bClosed; bClosed = false; return B; }

    void Update(UWorld* World, float Dt)
    {
        if (!bOpen) return;
        CurWorld = World;
        Accum += Dt;
        int Steps = 0;
        while (bOpen && Accum >= Step && Steps < 8)
        {
            Accum -= Step;
            Steps++;
            DropperInput::Update(float(Step));
            int Events[DropperInput::AptEventCount];
            const int N = DropperInput::Events(Events, DropperInput::AptEventCount);
            MsCarry += Step * 1000.0;   // whole milliseconds, the fraction carried
            const int Ms = int(MsCarry);
            MsCarry -= Ms;
            Tick(Ms);
            if (QInputBlockMs) QInputBlockMs = std::max(0, QInputBlockMs - Ms);
            if (QOutroMs && (QOutroMs -= std::min(QOutroMs, Ms)) == 0) QOutroDone();
            GroupPaintStep();
            if (N)
            {
                // which buttons the dropper got (diagnostics: 2026-10-10 "L1 does nothing")
                std::string Line = "[dropper] state " + std::to_string(State) + (bQOpen ? " (properties)" : "") + " events";
                bool bAny = false;
                for (int i = 0; i < N; i++)
                    if (Events[i] < DropperInput::AptUp || Events[i] >= DropperInput::AptStartReleased) { Line += " " + std::to_string(Events[i]); bAny = true; }
                if (DropperInput::TopLeftShoulderHeld()) Line += " (LB held)";
                if (bAny) Log(Line);
            }
            for (int i = 0; i < N && bOpen; i++)
            {
                if (PMovie) { PopupEvent(Events[i]); PopupFinish(); }   // the popup is on top
                else if (bQOpen) QuickEvent(Events[i]);                 // then the properties menu
                else HandleEvent(Events[i]);
            }
            if (!bOpen) break;
            DropperEditor::UpdateStep(World);
            if (Movie) Movie->Advance(Step);
            if (QMovie) QMovie->Advance(Step);
            if (PMovie) PMovie->Advance(Step);
        }
        if (Accum > Step) Accum = 0;
        if (bOpen && Steps) DropperEditor::FinishSteps();
        if (bOpen) DropperEditor::Present(Accum / Step);
    }

    namespace
    {
        Apt::Player* LayerMovie(int Layer) { return Layer == 0 ? Movie.get() : Layer == 1 ? QMovie.get() : Layer == 2 ? PMovie.get() : nullptr; }
    }

    void RenderLayer(int Layer, std::vector<Apt::DrawCmd>& Out)
    {
        Out.clear();
        Apt::Player* M = LayerMovie(Layer);
        if (!M) return;
        // Button glyphs are drawn where the movie drew each button's placeholder (in its place in the
        // drawing order is not needed: nothing in the movie overlaps them).
        std::vector<Apt::DrawCmd> Icons;
        std::vector<Apt::Instance*> Hidden;
        std::vector<std::pair<Apt::Instance*, Apt::Cx>> Recoloured;
        const Value R = M->RootObject();
        if (R.t == Value::Obj)
            if (auto I = R.o->inst.lock()) FindButtons(*M, *I, Apt::Mat{}, Apt::Cx{}, int(M->Textures().size()), Icons, Hidden, Recoloured);
        for (auto* H : Hidden) H->visible = false;
        M->Render(Out);
        for (auto* H : Hidden) H->visible = true;
        for (auto& Rc : Recoloured) Rc.first->cx = Rc.second;
        for (auto& D : Icons) Out.push_back(std::move(D));
    }

    void Render(std::vector<Apt::DrawCmd>& Out) { RenderLayer(0, Out); }

    const Apt::Player* LayerPlayer(int Layer) { return LayerMovie(Layer); }

    const Apt::Player* Player() { return Movie.get(); }
    const std::vector<std::string>& ButtonIcons() { return IconPaths; }

    void Cancel()
    {
        if (bOpen) Exit();
    }

    void ResetAllObjects()
    {
        if (bOpen) return;
        DropperWorld::RemoveAll();
    }
}
