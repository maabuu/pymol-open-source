/* 
A* -------------------------------------------------------------------
B* This file contains source code for the PyMOL computer program
C* copyright 1998-2000 by Warren Lyford Delano of DeLano Scientific. 
D* -------------------------------------------------------------------
E* It is unlawful to modify or remove this copyright notice.
F* -------------------------------------------------------------------
G* Please see the accompanying LICENSE file for further information. 
H* -------------------------------------------------------------------
I* Additional authors of this source file include:
-* sc
-* 
-*
Z* -------------------------------------------------------------------
*/


#include"os_std.h"
#include"os_gl.h"
#include"os_python.h"
#include"os_numpy.h"

#include"Util.h"
#include "pymol/utility.h"

#include"Word.h"
#include"main.h"
#include"Base.h"
#include"MemoryDebug.h"
#include"Err.h"
#include"Matrix.h"
#include"ListMacros.h"
#include"PyMOLObject.h"
#include"Scene.h"
#include"SceneRay.h"
#include"SceneMouse.h"
#include"ScenePicking.h"
#include"Ortho.h"
#include"Vector.h"
#include"ButMode.h"
#include"Control.h"
#include"Selector.h"
#include"Setting.h"
#include"Movie.h"
#include"MyPNG.h"
#include"P.h"
#include"Editor.h"
#include"Executive.h"
#include"Wizard.h"
#include"CGO.h"
#include"ObjectDist.h"
#include"ObjectGadget.h"
#include"Seq.h"
#include"Menu.h"
#include"View.h"
#include"ObjectSlice.h"
#include"Text.h"
#include"PyMOLOptions.h"
#include"PyMOL.h"
#include"PConv.h"
#include"ScrollBar.h"
#include "ShaderMgr.h"
#include "Feedback.h"
#include "GFXManager.h"

#ifdef _PYMOL_OPENVR
#include"OpenVRMode.h"
#endif

//#define _OPENVR_STEREO_DEBUG_VIEWS

#include <string>
#include <vector>
#include <algorithm>
#include <glm/vec3.hpp>
#include <glm/mat4x4.hpp>
#include <optional>
#include <utility>

static void glReadBufferError(PyMOLGlobals *G, GLenum b, GLenum e){
  PRINTFB(G, FB_OpenGL, FB_Warnings)
    " WARNING: glReadBuffer caused GL error 0x%04x\n", e ENDFB(G);
}
// TH 2013-11-01: glReadBuffer fails in JyMOL when picking, OSX 10.9, Intel Graphics
// for minor cases (i.e., png is called) this might get called outside of the main
// thread (from ExecutiveDrawNow()) in this case, just don't call glReadBuffer for 
// now, it should be ok because i believe it is used to figure out the size of the
// 3D window (using SceneImagePrepareImpl) in situations where the size gets changed.
#define glReadBuffer(b) {   int e; if (PIsGlutThread()) glReadBuffer(b); \
    if((e = glGetError())) glReadBufferError(G, b, e); }

#define cSliceMin 1.0F

#define SceneLineHeight 127
#define SceneTopMargin 0
#define SceneBottomMargin 3
#define SceneLeftMargin 3

/* Shared with ShaderMgr */
/** Coefficients from: http://3dtv.at/Knowhow/AnaglyphComparison_en.aspx */
/** Optimize the look and feel of anaglyph 3D */
/* the last mode is the 3x3 identity */
// matrices are column major
float anaglyphL_constants[6][9] = { { 0.299, 0.000, 0.000, 0.587, 0.000, 0.000, 0.114, 0.000, 0.000 },
				    { 0.299, 0.000, 0.000, 0.587, 0.000, 0.000, 0.114, 0.000, 0.000 },
				    { 1.000, 0.000, 0.000, 0.000, 0.000, 0.000, 0.000, 0.000, 0.000 },
				    { 0.299, 0.000, 0.000, 0.587, 0.000, 0.000, 0.114, 0.000, 0.000 },
				    { 0.000, 0.000, 0.000, 0.700, 0.000, 0.000, 0.300, 0.000, 0.000 },
				    { 1.000, 0.000, 0.000, 0.000, 1.000, 0.000, 0.000, 0.000, 1.000 } };

float anaglyphR_constants[6][9] = { { 0.000, 0.000, 0.299, 0.000, 0.000, 0.587, 0.000, 0.000, 0.114 },
				    { 0.000, 0.299, 0.299, 0.000, 0.587, 0.587, 0.000, 0.114, 0.114 },
				    { 0.000, 0.000, 0.000, 0.000, 1.000, 0.000, 0.000, 0.000, 1.000 },
				    { 0.000, 0.000, 0.000, 0.000, 1.000, 0.000, 0.000, 0.000, 1.000 },
				    { 0.000, 0.000, 0.000, 0.000, 1.000, 0.000, 0.000, 0.000, 1.000 },
				    { 1.000, 0.000, 0.000, 0.000, 1.000, 0.000, 0.000, 0.000, 1.000 } };

#define F2UI(a) (unsigned int) ((a) * 255.0)


/* allow up to 10 seconds at 30 FPS */

/* EXPERIMENTAL VOLUME RAYTRACING DATA */
extern float *rayDepthPixels;
extern int rayVolume, rayWidth, rayHeight;

static void SceneRestartPerfTimer(PyMOLGlobals * G);
#define SceneRotateWithDirty SceneRotate
static void SceneClipSetWithDirty(PyMOLGlobals * G, float front, float back, int dirty);

int SceneViewEqual(SceneViewType left, SceneViewType right)
{
  int i;
  for(i = 0; i < cSceneViewSize; i++) {
    if(fabs(left[i] - right[i]) > R_SMALL4)
      return false;
  }
  return true;
}

Rect2D GridSetRayViewport(GridInfo& I, int slot)
{
  Rect2D view{};
  if (slot)
    I.slot = slot + I.first_slot - 1;
  else
    I.slot = slot;
  /* if we are in grid mode, then prepare the grid slot viewport */
  if (slot < 0) {
    return I.cur_view;
  } else if (slot == 0) {
    view.offset = Offset2D{};
    view.extent.width = static_cast<std::uint32_t>(I.cur_view.extent.width / I.n_col);
    view.extent.height = static_cast<std::uint32_t>(I.cur_view.extent.height / I.n_row);
    if (I.n_col < I.n_row) {
      view.extent.width *= I.n_col;
      view.extent.height *= I.n_col;
    } else {
      view.extent.width *= I.n_row;
      view.extent.height *= I.n_row;
    }
    view.offset.x += I.cur_view.offset.x + (I.cur_view.extent.width - view.extent.width) / 2;
    view.offset.y += I.cur_view.offset.y;
  } else {
    int abs_grid_slot = slot - I.first_slot;
    int grid_col = abs_grid_slot % I.n_col;
    int grid_row = (abs_grid_slot / I.n_col);
    view.offset.x =
        static_cast<std::int32_t>((grid_col * I.cur_view.extent.width) / I.n_col);
    view.offset.y = static_cast<std::int32_t>(
        I.cur_view.extent.height - ((grid_row + 1) * I.cur_view.extent.height) / I.n_row);
    view.extent.width = ((grid_col + 1) * I.cur_view.extent.width) / I.n_col - view.offset.x;
    view.extent.height = static_cast<std::uint32_t>(
        (I.cur_view.extent.height - ((grid_row) *I.cur_view.extent.height) / I.n_row) - view.offset.y);
    view.offset.x += I.cur_view.offset.x;
    view.offset.y += I.cur_view.offset.y;
  }
  return view;
}

void GridUpdate(GridInfo * I, float asp_ratio, GridMode mode, int size)
{
  if (mode != GridMode::NoGrid) {
    I->size = size;
    I->mode = mode;
    {
      int n_row = 1;
      int n_col = 1;
      int r_size = size;
      while((n_row * n_col) < r_size) {
        float asp1 = asp_ratio * (n_row + 1.0) / n_col;
        float asp2 = asp_ratio * (n_row) / (n_col + 1.0);
        if(asp1 < 1.0F)
          asp1 = 1.0 / asp1;
        if(asp2 < 1.0F)
          asp2 = 1.0 / asp2;
        if(fabs(asp1) > fabs(asp2))
          n_col++;
        else
          n_row++;
      }

      // the above algorithm can generate a 3x2 grid for size=4, but we want a
      // 2x2 in that case.
      while ((n_col - 1) * n_row >= size && size) {
        n_col -= 1;
      }
      while ((n_row - 1) * n_col >= size && size) {
        n_row -= 1;
      }

      I->n_row = n_row;
      I->n_col = n_col;
    }
    if(I->size > 1) {
      I->active = true;
      I->asp_adjust = (float) I->n_row / I->n_col;
      I->first_slot = 1;
      I->last_slot = I->size;
    } else {
      I->active = false;
    }
  } else {
    I->active = false;
  }
}

void SceneInvalidateStencil(PyMOLGlobals * G)
{
  CScene *I = G->Scene;
  I->StencilValid = false;
}

int SceneGetGridSize(PyMOLGlobals* G, GridMode grid_mode)
{
  CScene* I = G->Scene;
  int size = 0;

  switch (grid_mode) {
  case GridMode::ByObject:
    if (I->m_slots.empty()) {
      I->m_slots.push_back(0);
    } else {
      std::fill(I->m_slots.begin(), I->m_slots.end(), 0);
    }
    {
      int max_slot = 0;
      for (auto& obj : I->Obj) {
        if (auto slot = obj->grid_slot) {
          max_slot = std::max(slot, max_slot);
          if (slot > 0) {
            VecCheck(I->m_slots, slot);
            I->m_slots[slot] = 1;
          }
        }
      }
      for (int slot = 0; slot <= max_slot; slot++) {
        if (I->m_slots[slot])
          I->m_slots[slot] = ++size;
      }
    }
    break;
  case GridMode::ByObjectStates:
  case GridMode::ByObjectByState:
    if (!I->m_slots.empty()) {
      I->m_slots.clear();
    }
    {
      int max_slot = 0;
      for (auto& obj : I->Obj) {
        auto slot = obj->getNFrame();
        if (grid_mode == GridMode::ByObjectByState) {
          obj->grid_slot = max_slot; // slot offset for 1st state
          max_slot += slot;
        } else if (max_slot < slot) {
          max_slot = slot;
        }
      }
      size = max_slot;
    }
    break;
  }
  auto grid_max = SettingGet<int>(G, cSetting_grid_max);
  if (grid_max >= 0)
    size = std::min(size, grid_max);
  return size;
}

int SceneHasImage(PyMOLGlobals * G)
{
  CScene *I = G->Scene;
  return (I->Image && !I->Image->empty());
}

int SceneMustDrawBoth(PyMOLGlobals * G)
{
  CScene *I = G->Scene;
  return (G->StereoCapable &&
          ((I->StereoMode == 1) ||
           SettingGetGlobal_b(G, cSetting_stereo_double_pump_mono)));
}

static int SceneDeferClickWhen(Block * block, int button, int x, int y, double when,
                               int mod);

int stereo_via_adjacent_array(int stereo_mode)
{
  switch (stereo_mode) {
  case cStereo_crosseye:
  case cStereo_walleye:
  case cStereo_sidebyside:
    return true;
  }
  return false;
}

int StereoIsAdjacent(PyMOLGlobals * G){
  CScene *I = G->Scene;
  return stereo_via_adjacent_array(I->StereoMode);
}

void SceneAbortAnimation(PyMOLGlobals * G)
{
  CScene *I = G->Scene;
  if(I->cur_ani_elem < I->n_ani_elem) { /* allow user to override animation */
    I->cur_ani_elem = I->n_ani_elem;
  }
}

void ScenePrimeAnimation(PyMOLGlobals * G)
{
  if(G->HaveGUI) {
    CScene *I = G->Scene;
    UtilZeroMem(I->ani_elem, sizeof(CViewElem));
    SceneToViewElem(G, I->ani_elem, nullptr);
    I->ani_elem[0].specification_level = 2;
    I->n_ani_elem = 0;
  }
}

static float SceneGetFPS(PyMOLGlobals * G)
{
  float fps = SettingGetGlobal_f(G, cSetting_movie_fps);
  float minTime;
  if(fps <= 0.0F) {
    if(fps < 0.0)
      minTime = 0.0;            /* negative fps means full speed */
    else                        /* 0 fps means use movie_delay instead */
      minTime = SettingGetGlobal_f(G, cSetting_movie_delay) / 1000.0;
    if(minTime >= 0.0F)
      fps = 1.0F / minTime;
    else
      fps = 1000.0F;
  }
  return fps;
}

/**
 * Release `G->Scene->Image` and clear related flags (`CopyType`).
 *
 * Invalidates the Ortho CGO and requests a redisplay. It's not entirely clear
 * why this is done, a code comment says "need to invalidate since text could be
 * shown".
 */
static void ScenePurgeImage(PyMOLGlobals * G)
{
  CScene *I = G->Scene;
  I->CopyType = false;
  I->Image = nullptr;

  // TODO does this belong here?
  if (true /* !noinvalid */)
    OrthoInvalidateDoDraw(G); // right now, need to invalidate since text could be shown
}

void SceneInvalidateCopy(PyMOLGlobals * G, int free_buffer)
{
  CScene *I = G->Scene;
  if(I) {
    if(free_buffer){
      ScenePurgeImage(G);
    }
    else{
      I->Image = nullptr;
    }
    if (I->CopyType)
      OrthoInvalidateDoDraw(G); // right now, need to invalidate since text could be shown
    I->CopyType = false;
  }
}

void SceneInvalidate(PyMOLGlobals * G)
{
  SceneInvalidateCopy(G, false);
  SceneDirty(G);
  PyMOL_NeedRedisplay(G->PyMOL);
}

void SceneLoadAnimation(PyMOLGlobals * G, double duration, int hand)
{
  if(G->HaveGUI) {
    double now;
    int target = (int) (duration * 30);
    CScene *I = G->Scene;
    if(target < 1)
      target = 1;
    if(target > MAX_ANI_ELEM)
      target = MAX_ANI_ELEM;
    UtilZeroMem(I->ani_elem + 1, sizeof(CViewElem) * target);
    SceneToViewElem(G, I->ani_elem + target, nullptr);
    I->ani_elem[target].specification_level = 2;
    now = UtilGetSeconds(G);
    I->ani_elem[0].timing_flag = true;
    I->ani_elem[0].timing = now + 0.01;
    I->ani_elem[target].timing_flag = true;
    I->ani_elem[target].timing = now + duration;
    ViewElemInterpolate(G, I->ani_elem, I->ani_elem + target,
                        2.0F, 1.0F, true, 0.0F, hand, 0.0F);
    SceneFromViewElem(G, I->ani_elem, true);
    I->cur_ani_elem = 0;
    I->n_ani_elem = target;
    I->AnimationStartTime = UtilGetSeconds(G);
    I->AnimationStartFlag = true;
    I->AnimationStartFrame = SceneGetFrame(G);
    I->AnimationLagTime = 0.0;
  }
}

/**
 * Set FrontSafe and BackSafe
 */
void UpdateFrontBackSafe(CScene *I)
{
  float front = I->m_view.m_clip().m_front;
  float back = I->m_view.m_clip().m_back;

  // minimum slab
  if(back - front < cSliceMin) {
    float avg = (back + front) / 2.0;
    back = avg + cSliceMin / 2.0;
    front = avg - cSliceMin / 2.0;
  }

  // minimum front
  if (front < cSliceMin) {
    front = cSliceMin;

    // minimum slab
    if (back < front + cSliceMin)
      back = front + cSliceMin;
  }

  I->m_view.m_clipSafe().m_front = front;
  I->m_view.m_clipSafe().m_back = back;
}

#define SELE_MODE_MAX 7

static const char SelModeKW[][20] = {
  "",
  "byresi",
  "bychain",
  "bysegi",
  "byobject",
  "bymol",
  "bca.",
};

static void SceneUpdateInvMatrix(PyMOLGlobals * G)
{
  // TODO: Test glm::inverse(I->m_view.rotMatrix())
  CScene *I = G->Scene;
  const auto rm = glm::value_ptr(I->m_view.rotMatrix());
  float *im = I->InvMatrix;
  im[0] = rm[0];
  im[1] = rm[4];
  im[2] = rm[8];
  im[3] = 0.0F;
  im[4] = rm[1];
  im[5] = rm[5];
  im[6] = rm[9];
  im[7] = 0.0F;
  im[8] = rm[2];
  im[9] = rm[6];
  im[10] = rm[10];
  im[11] = 0.0F;
  im[12] = 0.0F;
  im[13] = 0.0F;
  im[14] = 0.0F;
  im[15] = 1.0F;
}

void SceneUpdateStereo(PyMOLGlobals * G)
{
  SceneSetStereo(G, SettingGetGlobal_b(G, cSetting_stereo));
  PyMOL_NeedRedisplay(G->PyMOL);
}

/**
 * Get the selection operator for the `mouse_selection_mode` setting.
 * Example: `mouse_selection_mode=2` -> "bychain"
 */
const char *SceneGetSeleModeKeyword(PyMOLGlobals * G)
{
  int sel_mode = SettingGetGlobal_i(G, cSetting_mouse_selection_mode);
  if((sel_mode >= 0) && (sel_mode < SELE_MODE_MAX))
    return (char *) SelModeKW[sel_mode];
  return (char *) SelModeKW[0];
}

#ifdef _PYMOL_OPENVR
static float s_oldFov = -1.0f;
#endif

void SceneToViewElem(PyMOLGlobals * G, CViewElem * elem, const char *scene_name)
{
  double *dp;
  CScene *I = G->Scene;
  const auto& pos = I->m_view.pos();
  const auto& ori = I->m_view.origin();

  float dY = 0, dZ = 0;
  float fov = SettingGetGlobal_f(G, cSetting_field_of_view);
  float scale = 1.0f / I->Scale;

#ifdef _PYMOL_OPENVR
  if (I->StereoMode == cStereo_openvr) {
    float dist = fabsf(pos.z);
    float fovVR = fov;
    fov = s_oldFov;
    dY = scale * 1.0f;
    dZ = scale * dist * (tanf(fovVR * PI / 360.f) / tanf(fov * PI / 360.f) - 1.0f);
  }
#endif

  /* copy rotation matrix */
  elem->matrix_flag = true;
  dp = elem->matrix;
  auto fp = glm::value_ptr(I->m_view.rotMatrix());
  *(dp++) = (double) *(fp++);
  *(dp++) = (double) *(fp++);
  *(dp++) = (double) *(fp++);
  *(dp++) = (double) *(fp++);

  *(dp++) = (double) *(fp++);
  *(dp++) = (double) *(fp++);
  *(dp++) = (double) *(fp++);
  *(dp++) = (double) *(fp++);

  *(dp++) = (double) *(fp++);
  *(dp++) = (double) *(fp++);
  *(dp++) = (double) *(fp++);
  *(dp++) = (double) *(fp++);

  *(dp++) = 0.0F;
  *(dp++) = 0.0F;
  *(dp++) = 0.0F;
  *(dp++) = 1.0F;

  /* copy position */
  elem->pre_flag = true;
  dp = elem->pre;
  *(dp++) = (double) pos.x * scale;
  *(dp++) = (double) pos.y * scale - dY;
  *(dp++) = (double) pos.z * scale - dZ;

  /* copy origin (negative) */
  elem->post_flag = true;
  dp = elem->post;
  *(dp++) = (double) -ori.x;
  *(dp++) = (double) -ori.y;
  *(dp++) = (double) -ori.z;

  elem->clip_flag = true;
  elem->front = I->m_view.m_clip().m_front * scale + dZ;
  elem->back = I->m_view.m_clip().m_back * scale + dZ;

  elem->ortho_flag = true;
  elem->ortho = SettingGetGlobal_b(G, cSetting_ortho) ? fov : -fov;

  {
    if(elem->scene_flag && elem->scene_name) {
      OVLexicon_DecRef(G->Lexicon, elem->scene_name);
      elem->scene_name = 0;
      elem->scene_flag = 0;
    }
  }
  {
    if(!scene_name)
      scene_name = SettingGetGlobal_s(G, cSetting_scene_current_name);
    if(scene_name && scene_name[0]) {
      OVreturn_word result = OVLexicon_GetFromCString(G->Lexicon, scene_name);
      if(OVreturn_IS_OK(result)) {
        elem->scene_name = result.word;
        elem->scene_flag = true;
      }
    }
  }

#ifdef _OPENVR_STEREO_DEBUG_VIEWS
  printf("%-20s IP: %11lf %11lf %11lf, IF/IB: %11lf %11lf, IS: %11lf, IV: %11lf  ==>  EP: %11lf %11lf %11lf, EF/EB: %11lf %11lf, EV: %11lf\n",
    "SceneToViewElem",
    pos.x, pos.y, pos.z,
    I->m_view.m_clip().m_front, I->m_view.m_clip().m_back, I->Scale,
    SettingGetGlobal_f(G, cSetting_field_of_view),
    elem->pre[0], elem->pre[1], elem->pre[2],
    elem->front, elem->back,
    elem->ortho
  );
#endif // _OPENVR_STEREO_DEBUG_VIEWS
}

void SceneFromViewElem(PyMOLGlobals * G, CViewElem * elem, int dirty)
{
  CScene *I = G->Scene;
  float *fp;
  double *dp;
  int changed_flag = false;

  float dY = 0, dZ = 0;
  float fov = elem->ortho;
  float scale = I->Scale;
  auto pos = I->m_view.pos();
  auto ori = I->m_view.origin();
  auto rot = I->m_view.rotMatrix();

#ifdef _PYMOL_OPENVR
  if (I->StereoMode == cStereo_openvr) {
    float dist = fabsf(elem->pre[2]);
    float fovVR = SettingGetGlobal_f(G, cSetting_field_of_view);
    dY = -1.0f;
    dZ = scale * dist * (tanf(fabsf(fov) * PI / 360.f) / tanf(fovVR * PI / 360.f) - 1.0f);
    fov = fov < 0.0f ? -fovVR : fovVR;
  }
#endif

  if(elem->matrix_flag) {
    rot = glm::make_mat4(elem->matrix);
    changed_flag = true;
    SceneUpdateInvMatrix(G);
  }

  if(elem->pre_flag) {
    dp = elem->pre;
    fp = glm::value_ptr(pos);
    *(fp++) = (float) *(dp++) * scale;
    *(fp++) = (float) *(dp++) * scale - dY;
    *(fp++) = (float) *(dp++) * scale - dZ;
    changed_flag = true;
  }

  if(elem->post_flag) {
    dp = elem->post;
    fp = glm::value_ptr(ori);
    *(fp++) = (float) (-*(dp++));
    *(fp++) = (float) (-*(dp++));
    *(fp++) = (float) (-*(dp++));
    changed_flag = true;
  }

  if(elem->clip_flag) {
    SceneClipSetWithDirty(G, elem->front * scale + dZ, elem->back * scale + dZ, dirty);
  }

  if(elem->ortho_flag) {
    if(fov < 0.0F) {
      SettingSetGlobal_b(G, cSetting_ortho, 0);
      if(fov < -(1.0F - R_SMALL4)) {
        SettingSetGlobal_f(G, cSetting_field_of_view, -fov);
      }
    } else {
      SettingSetGlobal_b(G, cSetting_ortho, (fov > 0.5F));
      if(fov > (1.0F + R_SMALL4)) {
        SettingSetGlobal_f(G, cSetting_field_of_view, fov);
      }
    }
  }
  if(elem->state_flag&&!MovieDefined(G)) {
    SettingSetGlobal_i(G, cSetting_state, (elem->state)+1);
  }
  if(changed_flag) {
    SceneRestartSweepTimer(G);
    I->RockFrame = 0;
    SceneRovingDirty(G);
    I->m_view.setPos(pos);
    I->m_view.setOrigin(ori);
    I->m_view.setRotMatrix(rot);
  }

#ifdef _OPENVR_STEREO_DEBUG_VIEWS
  printf("%-20s IP: %11lf %11lf %11lf, IF/IB: %11lf %11lf, IS: %11lf, IV: %11lf  <==  EP: %11lf %11lf %11lf, EF/EB: %11lf %11lf, EV: %11lf\n",
    "SceneFromViewElem",
    pos.x, pos.y, pos.z,
    I->m_view.m_clip().m_front, I->m_view.m_clip().m_back, I->Scale,
    SettingGetGlobal_f(G, cSetting_field_of_view),
    elem->pre[0], elem->pre[1], elem->pre[2],
    elem->front, elem->back,
    elem->ortho
  );
#endif // _OPENVR_STEREO_DEBUG_VIEWS
}

SceneUnitContext ScenePrepareUnitContext(const Extent2D& extent)
{
  SceneUnitContext context{};
  float tw = 1.0F;
  float th = 1.0F;
  float aspRat = extent.height != 0
                     ? (extent.width / static_cast<float>(extent.height))
                     : 1.0f;

  if(aspRat > 1.0F) {
    tw = aspRat;
  } else {
    th = 1.0F / aspRat;
  }

  context.unit_left = (1.0F - tw) / 2;
  context.unit_right = (tw + 1.0F) / 2;
  context.unit_top = (1.0F - th) / 2;
  context.unit_bottom = (th + 1.0F) / 2;
  context.unit_front = -0.5F;
  context.unit_back = 0.5F;
  /*
     printf(
     "ScenePrepareUnitContext:%8.3f %8.3f %8.3f %8.3f %8.3f %8.3f\n",
     context->unit_left,
     context->unit_right,
     context->unit_top, 
     context->unit_bottom,
     context->unit_front,
     context->unit_back);
   */
  return context;
}

/**
 * cmd.get_viewport()
 */
void SceneGetWidthHeight(PyMOLGlobals * G, int *width, int *height)
{
  CScene *I = G->Scene;
  *width = I->Width;
  *height = I->Height;
}

Extent2D SceneGetExtent(PyMOLGlobals* G)
{
  return Extent2D{static_cast<std::uint32_t>(G->Scene->Width),
      static_cast<std::uint32_t>(G->Scene->Height)};
}

void SceneSetExtent(PyMOLGlobals* G, const Extent2D& extent)
{
  G->Scene->Width = extent.width;
  G->Scene->Height = extent.height;
}

Rect2D SceneGetRect(PyMOLGlobals* G)
{
  auto I = G->Scene;
  return Rect2D{Offset2D{I->rect.left, I->rect.bottom}, SceneGetExtent(G)};
}

float SceneGetAspectRatio(PyMOLGlobals* G)
{
  auto extent = SceneGetExtent(G);
  return static_cast<float>(extent.width) / static_cast<float>(extent.height);
}

/**
 * Get the actual current (sub-)viewport size, considering grid mode and
 * side-by-side stereo
 */
Extent2D SceneGetExtentStereo(PyMOLGlobals* G)
{
  auto I = G->Scene;

  if (I->grid.active) {
    // TODO: this considers "draw W, H" (PYMOL-2775)
    return I->grid.cur_viewport_size;
  }

  Extent2D extent{static_cast<std::uint32_t>(I->Width),
      static_cast<std::uint32_t>(I->Height)};
  // TODO: this does NOT consider "draw W, H" (PYMOL-2775)
  if (stereo_via_adjacent_array(I->StereoMode))
    extent.width /= 2.f;
  return extent;
}

void SceneSetCardInfo(PyMOLGlobals * G,
    const char *vendor,
    const char *renderer,
    const char *version)
{
  CScene *I = G->Scene;
  if (!vendor) vendor = "(null)";
  if (!renderer) renderer = "(null)";
  if (!version) version = "(null)";
  UtilNCopy(I->vendor, vendor, sizeof(OrthoLineType) - 1);
  UtilNCopy(I->renderer, renderer, sizeof(OrthoLineType) - 1);
  UtilNCopy(I->version, version, sizeof(OrthoLineType) - 1);
}

int SceneGetStereo(PyMOLGlobals * G)
{
  CScene *I = G->Scene;
  return (I->StereoMode);
}

void SceneGetCardInfo(PyMOLGlobals * G, char **vendor, char **renderer, char **version)
{
  CScene *I = G->Scene;
  (*vendor) = I->vendor;
  (*renderer) = I->renderer;
  (*version) = I->version;
}

void SceneSuppressMovieFrame(PyMOLGlobals * G)
{
  CScene *I = G->Scene;
  I->MovieFrameFlag = false;
}

/**
 * Get center of screen in world coordinates
 *
 * @param[out] pos 3f output vector
 */
void SceneGetCenter(PyMOLGlobals * G, float *pos)
{
  CScene *I = G->Scene;
  auto camPos = I->m_view.pos();
  const auto& ori = I->m_view.origin();

  MatrixTransformC44fAs33f3f(glm::value_ptr(I->m_view.rotMatrix()), glm::value_ptr(ori), pos);

  pos[0] -= camPos.x;
  pos[1] -= camPos.y;

  MatrixInvTransformC44fAs33f3f(glm::value_ptr(I->m_view.rotMatrix()), pos, pos);
}

/*========================================================================*/
int SceneGetNFrame(PyMOLGlobals * G, int *has_movie)
{
  CScene *I = G->Scene;
  if(has_movie)
    *has_movie = I->HasMovie;
  return (I->NFrame);
}

/*========================================================================*/
/**
   Get information required to define the geometry
   of a particular view, for shipping to and from python
   as a list of floats
   @verbatim
   0-15 = 4x4 rotation matrix 
   16-18 = position
   19-21 = origin
   22    = front plane
   23    = rear plane
   24    = orthoscopic flag 
   @endverbatim
   @param[out] view buffer to fill
*/
void SceneGetView(PyMOLGlobals * G, SceneViewType view)
{
  float *p;
  CScene *I = G->Scene;
  p = view;

  float dY = 0, dZ = 0;
  float fov = SettingGetGlobal_f(G, cSetting_field_of_view);
  float scale = 1.0f / I->Scale;
  const auto& pos = I->m_view.pos();
  const auto& ori = I->m_view.origin();

#ifdef _PYMOL_OPENVR
  if (I->StereoMode == cStereo_openvr) {
    float dist = fabsf(pos.z);
    float fovVR = fov;
    fov = s_oldFov;
    dY = scale * 1.0f;
    dZ = scale * dist * (tanf(fovVR * PI / 360.f) / tanf(fov * PI / 360.f) - 1.0f);
  }
#endif

  std::copy_n(glm::value_ptr(I->m_view.rotMatrix()), 16, p);
  p += 16;
  *(p++) = pos.x * scale;
  *(p++) = pos.y * scale - dY;
  *(p++) = pos.z * scale - dZ;
  *(p++) = ori.x;
  *(p++) = ori.y;
  *(p++) = ori.z;
  *(p++) = I->m_view.m_clip().m_front * scale + dZ;
  *(p++) = I->m_view.m_clip().m_back * scale + dZ;
  *(p++) = SettingGetGlobal_b(G, cSetting_ortho) ? fov : -fov;

#ifdef _OPENVR_STEREO_DEBUG_VIEWS
  printf("%-20s IP: %11lf %11lf %11lf, IF/IB: %11lf %11lf, IS: %11lf, IV: %11lf  ==>  EP: %11lf %11lf %11lf, EF/EB: %11lf %11lf, EV: %11lf\n",
    "SceneGetView",
    pos.x, pos.y, pos.z,
    I->m_view.m_clip().m_front, I->m_view.m_clip().m_back, I->Scale,
    SettingGetGlobal_f(G, cSetting_field_of_view),
    view[16], view[17], view[18],
    view[22], view[23],
    view[24]
  );
#endif // _OPENVR_STEREO_DEBUG_VIEWS
}

/*========================================================================*/
void SceneSetView(PyMOLGlobals * G, const SceneViewType view,
                  int quiet, float animate, int hand)
{
  const float *p;
  CScene *I = G->Scene;

  if(animate < 0.0F) {
    if(SettingGetGlobal_b(G, cSetting_animation))
      animate = SettingGetGlobal_f(G, cSetting_animation_duration);
    else
      animate = 0.0F;
  }
  if(animate != 0.0F)
    ScenePrimeAnimation(G);
  else {
    SceneAbortAnimation(G);
  }

  float dY = 0, dZ = 0;
  float fov = view[24];
  float scale = I->Scale;

#ifdef _PYMOL_OPENVR
  if (I->StereoMode == cStereo_openvr) {
    float dist = fabsf(view[18]);
    float fovVR = SettingGetGlobal_f(G, cSetting_field_of_view);
    dY = -1.0f;
    dZ = scale * dist * (tanf(fabsf(fov) * PI / 360.f) / tanf(fovVR * PI / 360.f) - 1.0f);
    fov = fov < 0.0f ? -fovVR : fovVR;
  }
#endif

  p = view;
  I->m_view.setRotMatrix(glm::make_mat4(p));
  p += 16;
  SceneUpdateInvMatrix(G);
  auto newX = *(p++) * scale;
  auto newY = *(p++) * scale - dY;
  auto newZ = *(p++) * scale - dZ;
  I->m_view.setPos(newX, newY, newZ);
  auto newOriX = *(p++);
  auto newOriY = *(p++);
  auto newOriZ = *(p++);
  I->m_view.setOrigin(newOriX, newOriY, newOriZ);

  I->LastSweep = 0.0F;
  I->LastSweepX = 0.0F;
  I->LastSweepY = 0.0F;
  I->SweepTime = 0.0;
  I->RockFrame = 0;

  SceneClipSet(G, p[0] * scale + dZ, p[1] * scale + dZ);

  p += 2;
  if(fov < 0.0F) {
    SettingSetGlobal_b(G, cSetting_ortho, 0);
    if(fov < -(1.0F - R_SMALL4)) {
      SettingSetGlobal_f(G, cSetting_field_of_view, -fov);
    }
  } else {
    SettingSetGlobal_b(G, cSetting_ortho, (fov > 0.5F));
    if(fov > (1.0F + R_SMALL4)) {
      SettingSetGlobal_f(G, cSetting_field_of_view, fov);
    }
  }
  if(!quiet) {
    PRINTFB(G, FB_Scene, FB_Actions)
      " Scene: view updated.\n" ENDFB(G);
  }

#ifdef _OPENVR_STEREO_DEBUG_VIEWS
  const auto& pos = I->m_view.pos();
  printf("%-20s IP: %11lf %11lf %11lf, IF/IB: %11lf %11lf, IS: %11lf, IV: %11lf  <==  EP: %11lf %11lf %11lf, EF/EB: %11lf %11lf, EV: %11lf\n",
    "SceneSetView",
    pos.x, pos.y, pos.z,
    I->m_view.m_clip().m_front, I->m_view.m_clip().m_back, I->Scale,
    SettingGetGlobal_f(G, cSetting_field_of_view),
    view[16], view[17], view[18],
    view[22], view[23],
    view[24]
  );
#endif // _OPENVR_STEREO_DEBUG_VIEWS

  if(animate != 0.0F)
    SceneLoadAnimation(G, animate, hand);

  SceneRovingDirty(G);
}


/*========================================================================*/
void SceneDontCopyNext(PyMOLGlobals * G)

/* disables automatic copying of the image for the next rendering run */
{
  CScene *I = G->Scene;
  I->CopyNextFlag = false;
}


/*========================================================================*/
void SceneUpdateStereoMode(PyMOLGlobals * G)
{
  if(G->Scene->StereoMode) {
    SceneSetStereo(G, true);
  }
}

#ifdef _PYMOL_OPENVR
/*========================================================================*/
static
void ResetFovWidth(PyMOLGlobals * G, bool enableOpenVR, float fovNew) {
  CScene *I = G->Scene;
  auto pos = I->m_view.pos();

#ifdef _OPENVR_STEREO_DEBUG_VIEWS
  printf("%-20s IP: %11lf %11lf %11lf, IF/IB: %11lf %11lf, IS: %11lf  ==>\n",
    "ResetFovWidth BEFORE",
    pos.x, pos.y, pos.z,
    I->m_view.m_clip().m_front, I->m_view.m_clip().m_back, I->Scale
  );
#endif // _OPENVR_STEREO_DEBUG_VIEWS

  float fovOld = SettingGetGlobal_f(G, cSetting_field_of_view);
  SettingSetGlobal_f(G, cSetting_field_of_view, fovNew);

  const float tanOld = tanf(fovOld * PI / 360.f);
  const float tanNew = tanf(fovNew * PI / 360.f);

  const float distOld = fabsf(pos.z);

  const float scaleOld = I->Scale;
  const float scaleNew = I->Scale = enableOpenVR ? 1.0f / (distOld * tanOld) : 1.0f;
  const float scale = scaleNew / scaleOld;

  auto newX = pos.x * scale;
  auto newY = enableOpenVR ? pos.y * scale + 1.0f : (pos.y - 1.0f) * scale;
  auto newZ = pos.z * scale * tanOld / tanNew;

  const float dZ = fabsf(pos.z) - distOld * scale;
  I->m_view.m_clip().m_front = I->m_view.m_clip().m_front * scale + dZ;
  I->m_view.m_clip().m_back = I->m_view.m_clip().m_back * scale + dZ;

#ifdef _OPENVR_STEREO_DEBUG_VIEWS
  printf("%-20s IP: %11lf %11lf %11lf, IF/IB: %11lf %11lf, IS: %11lf  <==\n",
    "ResetFovWidth AFTER",
    pos.x, pos.y, pos.z,
    I->m_view.m_clip().m_front, I->m_view.m_clip().m_back, I->Scale
  );
#endif // _OPENVR_STEREO_DEBUG_VIEWS
  I->m_view.setPos(pos);
  UpdateFrontBackSafe(I);
  SceneRovingDirty(G);
}

/*========================================================================*/
static
void SceneResetOpenVRSettings(PyMOLGlobals * G, bool enableOpenVR) { 
  // set FOV = 110 for openVR
  float openVRFov = 110.0 * 0.5;

  // old camera props
  static bool commonCorrect = false;
  if (s_oldFov < 0.0f)
    s_oldFov = SettingGetGlobal_f(G, cSetting_field_of_view);

  if (enableOpenVR) {
    s_oldFov =  SettingGetGlobal_f(G, cSetting_field_of_view);
    ResetFovWidth(G, enableOpenVR, openVRFov);
    commonCorrect = true;
    SettingSetGlobal_f(G, cSetting_dynamic_width_factor, 0.004f); // for correct line width in lines mode
  } else if (commonCorrect){
    ResetFovWidth(G, enableOpenVR, s_oldFov);
    commonCorrect = false;
    SettingSetGlobal_f(G, cSetting_dynamic_width_factor, 0.06f);
  }
}
#endif

/*========================================================================*/
void SceneSetStereo(PyMOLGlobals * G, bool flag)
{
  CScene *I = G->Scene;
  int cur_stereo_mode = I->StereoMode;

  if(flag) {
    I->StereoMode = SettingGetGlobal_i(G, cSetting_stereo_mode);
  } else {
    I->StereoMode = 0;
  }
  
  SettingSetGlobal_b(G, cSetting_stereo, flag);

  if (cur_stereo_mode != I->StereoMode) {
    if (cur_stereo_mode == cStereo_geowall ||
        I->StereoMode == cStereo_geowall) {
      OrthoReshape(G, G->Option->winX, G->Option->winY, true);
    }

#ifdef _PYMOL_OPENVR
    // enter or leave OpenVR mode
    if (I->StereoMode == cStereo_openvr || cur_stereo_mode == cStereo_openvr) {
      bool enableOpenVR = I->StereoMode == cStereo_openvr;

      // reset camera position
      SceneResetOpenVRSettings(G, enableOpenVR);

      // force open internal menu
      PParse(G, "cmd.set_wizard_stack()");
      if (enableOpenVR) {
        PParse(G, "wizard openvr");
      }
    }
#endif

    SceneInvalidateStencil(G);
    SceneInvalidate(G);
    G->ShaderMgr->Set_Reload_Bits(RELOAD_VARIABLES);
  }
}


/*========================================================================*/
void SceneTranslate(PyMOLGlobals * G, float x, float y, float z)
{
  CScene *I = G->Scene;
  I->m_view.translate(x, y, z);
  SceneClipSet(G, I->m_view.m_clip().m_front - z, I->m_view.m_clip().m_back - z);
}

void SceneTranslateScaled(PyMOLGlobals * G, float x, float y, float z, int sdof_mode)
{
  CScene *I = G->Scene;
  int invalidate = false;

  switch (sdof_mode) {
  case SDOF_NORMAL_MODE:
    if((x != 0.0F) || (y != 0.0F)) {
      float vScale = SceneGetExactScreenVertexScale(G, nullptr);
      float factor = vScale * (I->Height + I->Width) / 2;
      I->m_view.translate(x * factor, y * factor, 0.0f);
      invalidate = true;
    }
    if(z != 0.0F) {
      float factor = ((I->m_view.m_clipSafe().m_front + I->m_view.m_clipSafe().m_back) / 2);        /* average distance within visible space */
      if(factor > 0.0F) {
        factor *= z;
        I->m_view.translate(0.0f, 0.0f, factor);
        I->m_view.m_clip().m_front -= factor;
        I->m_view.m_clip().m_back -= factor;
        UpdateFrontBackSafe(I);
      }
      invalidate = true;
    }
    break;
  case SDOF_CLIP_MODE:
    if((x != 0.0F) || (y != 0.0F)) {
      float vScale = SceneGetExactScreenVertexScale(G, nullptr);
      float factor = vScale * (I->Height + I->Width) / 2;
      I->m_view.translate(x * factor, y * factor, 0.0f);
      invalidate = true;
    }
    if(z != 0.0F) {
      float factor = ((I->m_view.m_clipSafe().m_front + I->m_view.m_clipSafe().m_back) / 2);        /* average distance within visible space */
      if(factor > 0.0F) {
        factor *= z;
        {
          float old_front = I->m_view.m_clip().m_front;
          float old_back = I->m_view.m_clip().m_back;
          float old_origin = -I->m_view.pos().z;
          SceneClip(G, SceneClipMode::Linear, factor, nullptr, 0);
          SceneDoRoving(G, old_front, old_back, old_origin, true, true);
        }
        invalidate = true;
      }
    }
    break;
  case SDOF_DRAG_MODE:
    {
      float v2[3];
      float scale = SettingGetGlobal_f(G, cSetting_sdof_drag_scale);

      {
        /* when dragging, we treat all axes proportionately */
        float vScale = SceneGetExactScreenVertexScale(G, nullptr);
        float factor = vScale * (I->Height + I->Width) / 2;
        x *= factor;
        y *= factor;
        z *= factor;
      }

      v2[0] = x * scale;
      v2[1] = y * scale;
      v2[2] = z * scale;

      /* transform into model coodinate space */
      MatrixInvTransformC44fAs33f3f(glm::value_ptr(I->m_view.rotMatrix()), v2, v2);

      EditorDrag(G, nullptr, -1, cButModeMovDrag,
                 SettingGetGlobal_i(G, cSetting_state) - 1, nullptr, v2, nullptr);
    }
    break;
  }
  if(invalidate) {
    SceneInvalidate(G);
    if(SettingGetGlobal_b(G, cSetting_roving_origin)) {
      float v2[3];
      SceneGetCenter(G, v2);       /* gets position of center of screen */
      SceneOriginSet(G, v2, true);
    }
    if(SettingGetGlobal_b(G, cSetting_roving_detail)) {
      SceneRovingPostpone(G);
    }
  }
}

void SceneRotateScaled(PyMOLGlobals * G, float rx, float ry, float rz, int sdof_mode)
{
  CScene *I = G->Scene;
  int invalidate = false;
  float axis[3];
  switch (sdof_mode) {
  case SDOF_NORMAL_MODE:
    axis[0] = rx;
    axis[1] = ry;
    axis[2] = rz;
    {
      float angle = length3f(axis);
      normalize3f(axis);
      SceneRotate(G, 60 * angle, axis[0], axis[1], axis[2]);
    }
    break;
  case SDOF_CLIP_MODE:
    if((fabs(rz) > fabs(rx)) || (fabs(rz) > fabs(rx))) {
      rx = 0.0;
      ry = 0.0;
    } else {
      rz = 0.0;
    }
    axis[0] = rx;
    axis[1] = ry;
    axis[2] = 0.0;
    {
      float angle = length3f(axis);
      normalize3f(axis);
      SceneRotate(G, 60 * angle, axis[0], axis[1], axis[2]);
    }
    if(axis[2] != rz) {
      SceneClip(G, SceneClipMode::Scaling, 1.0F + rz, nullptr, 0);
    }
    break;
  case SDOF_DRAG_MODE:
    {
      float scale = SettingGetGlobal_f(G, cSetting_sdof_drag_scale);
      float v1[3], v2[3];
      axis[0] = rx;
      axis[1] = ry;
      axis[2] = rz;

      EditorReadyDrag(G, SettingGetGlobal_i(G, cSetting_state) - 1);

      {
        float angle = length3f(axis);
        normalize3f(axis);

        v1[0] = cPI * (60 * angle / 180.0F) * scale;

        /* transform into model coodinate space */
        MatrixInvTransformC44fAs33f3f(glm::value_ptr(I->m_view.rotMatrix()), axis, v2);

        EditorDrag(G, nullptr, -1, cButModeRotDrag,
                   SettingGetGlobal_i(G, cSetting_state) - 1, v1, v2, nullptr);
        invalidate = true;
      }
    }
    break;
  }
  if(invalidate) {
    SceneInvalidate(G);
  }
}


/*========================================================================*/

static void SceneClipSetWithDirty(PyMOLGlobals * G, float front, float back, int dirty)
{
  CScene *I = G->Scene;

  // minimum slab
  float minSlab = cSliceMin * I->Scale;
  if(back - front < minSlab) {
    float avg = (back + front) / 2.0;
    back = avg + minSlab / 2.0;
    front = avg - minSlab / 2.0;
  }

  I->m_view.m_clip().m_front = front;
  I->m_view.m_clip().m_back = back;

  UpdateFrontBackSafe(I);

  if(dirty)
    SceneInvalidate(G);
  else
    SceneInvalidateCopy(G, false);
}


/*========================================================================*/
void SceneClipSet(PyMOLGlobals * G, float front, float back)
{
  SceneClipSetWithDirty(G, front, back, true);
}


/*========================================================================*/
static SceneClipMode SceneClipGetEnum(pymol::zstring_view mode)
{
  static const std::unordered_map<pymol::zstring_view, SceneClipMode> modes{
      {"near", SceneClipMode::Near},
      {"far", SceneClipMode::Far},
      {"move", SceneClipMode::Move},
      {"slab", SceneClipMode::Slab},
      {"atoms", SceneClipMode::Atoms},
      {"scaling", SceneClipMode::Scaling},
      {"linear", SceneClipMode::Linear},
      {"near_set", SceneClipMode::Near_Set},
      {"far_set", SceneClipMode::Far_Set},
  };

  auto it = modes.find(mode);
  return (it == modes.end()) ? SceneClipMode::Invalid : it->second;
}

pymol::Result<> SceneClipFromMode(PyMOLGlobals* G, pymol::zstring_view mode, float movement,
    pymol::zstring_view sele, int state)
{
  auto plane = SceneClipGetEnum(mode);
  if (plane == SceneClipMode::Invalid) {
    return pymol::Error("invalid clip mode");
  }
  SceneClip(G, plane, movement, sele.c_str(), state);
  return {};
}

/*========================================================================*/
void SceneClip(PyMOLGlobals * G, SceneClipMode mode, float movement, const char *sele, int state)
{                               /* 0=front, 1=back */
  CScene *I = G->Scene;
  float avg;
  float mn[3], mx[3], cent[3], v0[3], offset[3], origin[3];
  const auto& pos = I->m_view.pos();
  switch (mode) {
  case SceneClipMode::Near:
    SceneClipSet(G, I->m_view.m_clip().m_front - movement, I->m_view.m_clip().m_back);
    break;
  case SceneClipMode::Far:
    SceneClipSet(G, I->m_view.m_clip().m_front, I->m_view.m_clip().m_back - movement);
    break;
  case SceneClipMode::Move:
    SceneClipSet(G, I->m_view.m_clip().m_front - movement, I->m_view.m_clip().m_back - movement);
    break;
  case SceneClipMode::Slab:
    if(sele[0]) {
      if(!ExecutiveGetExtent(G, sele, mn, mx, true, state, false))
        sele = nullptr;
      else {
        average3f(mn, mx, cent);        /* get center of selection */
        subtract3f(cent, glm::value_ptr(I->m_view.origin()), v0);        /* how far from origin? */
        MatrixTransformC44fAs33f3f(glm::value_ptr(I->m_view.rotMatrix()), v0, offset);   /* convert to view-space */
      }
    } else {
      sele = nullptr;
    }
    avg = (I->m_view.m_clip().m_front + I->m_view.m_clip().m_back) / 2.0F;
    movement /= 2.0F;
    if(sele) {
      avg = -pos.z - offset[2];
    }
    SceneClipSet(G, avg - movement, avg + movement);
    break;
  case SceneClipMode::Atoms:
    if(!sele)
      sele = cKeywordAll;
    else if(!sele[0]) {
      sele = cKeywordAll;
    }
    if(WordMatchExact(G, sele, cKeywordCenter, true)) {
      MatrixTransformC44fAs33f3f(glm::value_ptr(I->m_view.rotMatrix()), glm::value_ptr(I->m_view.origin()), origin);      /* convert to view-space */
      SceneClipSet(G, origin[2] - movement, origin[2] + movement);
    } else if(WordMatchExact(G, sele, cKeywordOrigin, true)) {
      SceneClipSet(G, -pos.z - movement, -pos.z + movement);
    } else {
      if(!ExecutiveGetCameraExtent(G, sele, mn, mx, true, state))
        sele = nullptr;
      if(sele) {
        if(sele[0]) {
          average3f(mn, mx, cent);      /* get center of selection */
          MatrixTransformC44fAs33f3f(glm::value_ptr(I->m_view.rotMatrix()), glm::value_ptr(I->m_view.origin()), origin);  /* convert to view-space */
          subtract3f(mx, origin, mx);   /* how far from origin? */
          subtract3f(mn, origin, mn);   /* how far from origin? */
          SceneClipSet(G, -pos.z - mx[2] - movement, -pos.z - mn[2] + movement);
        } else {
          sele = nullptr;
        }
      }
    }
    break;
  case SceneClipMode::Scaling:
    {
      double avg = (I->m_view.m_clip().m_front / 2.0) + (I->m_view.m_clip().m_back / 2.0);
      double width_half = I->m_view.m_clip().m_back - avg;
      double new_w_half = std::min(movement * width_half,
          width_half + 1000.0); // prevent exploding of clipping planes

      SceneClipSet(G, avg - new_w_half, avg + new_w_half);
    }
    break;
  case SceneClipMode::Proportional:
    {
      float shift = (I->m_view.m_clip().m_front - I->m_view.m_clip().m_back) * movement;
      SceneClipSet(G, I->m_view.m_clip().m_front + shift, I->m_view.m_clip().m_back + shift);
    }
    break;
  case SceneClipMode::Linear:
    {
      SceneClipSet(G, I->m_view.m_clip().m_front + movement, I->m_view.m_clip().m_back + movement);
    }
    break;
  case SceneClipMode::Near_Set:
    {
      SceneClipSet(G, movement, I->m_view.m_clip().m_back);
    }
    break;
  case SceneClipMode::Far_Set:
    {
      SceneClipSet(G, I->m_view.m_clip().m_front, movement);
    }
    break;
  }
}

/**
 * Retrieves clipping plane distances from camera
 * @return near and far clipping plane distances
 * @TODO: Needs camera param index when multiple cameras are implemented
 * @TODO: Return error when camera idx found.
 */
pymol::Result<std::pair<float, float>> SceneGetClip(PyMOLGlobals* G)
{
  auto clip = G->Scene->getSceneView().m_clip;
  return std::make_pair(clip.m_front, clip.m_back);
}

/*========================================================================*/
void SceneSetMatrix(PyMOLGlobals * G, float *m)
{
  CScene *I = G->Scene;
  I->m_view.setRotMatrix(glm::make_mat4(m));
  SceneUpdateInvMatrix(G);
}


/*========================================================================*/
void SceneGetViewNormal(PyMOLGlobals * G, float *v)
{
  CScene *I = G->Scene;
  copy3f(I->ViewNormal, v);
}


/*========================================================================*/
int SceneGetState(PyMOLGlobals * G)
{
  return (SettingGetGlobal_i(G, cSetting_state) - 1);
}


/*========================================================================*/
float *SceneGetMatrix(PyMOLGlobals * G)
{
  return const_cast<float*>(glm::value_ptr(G->Scene->m_view.rotMatrix()));
}

float *SceneGetPmvMatrix(PyMOLGlobals * G)
{
  CScene *I = G->Scene;
  multiply44f44f44f(SceneGetModelViewMatrixPtr(G),
      SceneGetProjectionMatrixPtr(G), I->PmvMatrix);
  return (I->PmvMatrix);
}


/*========================================================================*/

GLFramebufferConfig SceneDrawBothGetConfig(PyMOLGlobals* G)
{
  GLFramebufferConfig config;
  config.framebuffer = G->ShaderMgr->defaultBackbuffer.framebuffer;
  config.drawBuffer = SceneMustDrawBoth(G)
                          ? GL_BACK_LEFT
                          : G->ShaderMgr->defaultBackbuffer.drawBuffer;
  return config;
}

int SceneCaptureWindow(PyMOLGlobals * G)
{
  CScene *I = G->Scene;
  int ok = true;

  /* check assumptions */

  if(ok && G->HaveGUI && G->ValidContext) {
    auto drawBuffer = SceneDrawBothGetConfig(G);

    ScenePurgeImage(G);

    SceneCopy(G, drawBuffer, true, true);

    if(!I->Image)
      ok = false;

    if(ok && I->Image) {
      I->DirtyFlag = false;
      I->CopyType = 2;          /* suppresses display of copied image */
      if(SettingGetGlobal_b(G, cSetting_opaque_background))
        I->Image->m_needs_alpha_reset = true;
    }
  } else {
    ok = false;
  }
  return ok;
}

/**
 * Sets the Scene's cached copy image
 * @param image the image to set
 * @param dirty whether the image is dirty (needed?)
 * @param copyForced whether the image was forced to be copied
 */
static void SceneSetCopyImage(
    PyMOLGlobals* G, pymol::Image image, bool dirty, bool copyForced)
{
  auto I = G->Scene;
  I->Image = std::make_shared<pymol::Image>(std::move(image));
  I->CopyType = true;
  I->CopyForced = copyForced;
  I->DirtyFlag = dirty;
}

/**
 * Get the maximum dimensions of the viewport
 * @return the maximum dimensions of OpenGL viewport
 */
Extent2D SceneGLGetMaxDimensions(PyMOLGlobals* G)
{
  //TODO: Move this function to OpenGL Context manager
  GLint dims[2];
  glGetIntegerv(GL_MAX_VIEWPORT_DIMS, reinterpret_cast<GLint*>(&dims));
  return Extent2D{static_cast<std::uint32_t>(dims[0]), static_cast<std::uint32_t>(dims[1])};
}

Extent2D ExtentClampByAspectRatio(Extent2D extent, const Extent2D& maxDim)
{
  float extentAspect = static_cast<float>(extent.width) / extent.height;
  if (extent.width > maxDim.width) {
    extent.height = static_cast<std::uint32_t>(maxDim.width / extentAspect);
    extent.width = maxDim.width;
  }
  if (extent.height > maxDim.height) {
    extent.width = static_cast<std::uint32_t>(maxDim.height * extentAspect);
    extent.height = maxDim.height;
  }
  return extent;
}

/**
 * @brief Returns supersampled image
 * @param src source image
 * @param factor supersampling factor
 * @param shift shift value
 * @return supersampled image
 */
static pymol::Image SceneSupersampleImage(
    const pymol::Image& src, const UpscaledExtentInfo& upscaledExtent)
{
  auto factor = upscaledExtent.factor;
  auto shift = upscaledExtent.shift;
  auto oriWidth = src.getWidth();
  auto oriHeight = src.getHeight();
  unsigned int src_row_bytes = oriWidth * pymol::Image::getPixelSize();

  auto width = oriWidth / factor;
  auto height = oriHeight / factor;

  auto* p = src.bits();
  pymol::Image newImg(width, height);
  unsigned char* q = newImg.bits();
  unsigned int factor_col_bytes = factor * pymol::Image::getPixelSize();
  unsigned int factor_row_bytes = factor * src_row_bytes;

  // TODO: Rename variables and cleanup -- This was pulled almost 1:1
  for (int b = 0; b < height; b++) { /* rows */
    auto* pp = p;
    for (int a = 0; a < width; a++) { /* cols */
      unsigned int c1{};
      unsigned int c2{};
      unsigned int c3{};
      unsigned int c4{};
      auto* ppp = pp;
      for (int d = 0; d < factor; d++) { /* box rows */
        auto* pppp = ppp;
        for (int c = 0; c < factor; c++) { /* box cols */
          unsigned int alpha = pppp[3];
          c4 += alpha;
          c1 += *(pppp++) * alpha;
          c2 += *(pppp++) * alpha;
          c3 += *(pppp++) * alpha;
          pppp++;
        }
        ppp += src_row_bytes;
      }
      if (c4) { /* divide out alpha channel & average */
        c1 = c1 / c4;
        c2 = c2 / c4;
        c3 = c3 / c4;
      } else { /* alpha zero! so compute average RGB */
        c1 = c2 = c3 = 0;
        ppp = pp;
        for (int d = 0; d < factor; d++) { /* box rows */
          auto* pppp = ppp;
          for (int c = 0; c < factor; c++) { /* box cols */
            c1 += *(pppp++);
            c2 += *(pppp++);
            c3 += *(pppp++);
            pppp++;
          }
          ppp += src_row_bytes;
        }
        c1 = c1 >> shift;
        c2 = c2 >> shift;
        c3 = c3 >> shift;
      }
      *(q++) = c1;
      *(q++) = c2;
      *(q++) = c3;
      *(q++) = c4 >> shift;
      pp += factor_col_bytes;
    }
    p += factor_row_bytes;
  }
  return newImg;
}

pymol::Image GLImageToPyMOLImage(
    PyMOLGlobals* G, const GLFramebufferConfig& config, const Rect2D& srcRect)
{
  auto imgData = G->ShaderMgr->readPixelsFrom(G, srcRect, config);
  pymol::Image img(srcRect.extent.width, srcRect.extent.height);
  if (!imgData.empty()) {
    img.setVecData(std::move(imgData));
  }
  return img;
}

void PyMOLImageCopy(const pymol::Image& srcImage,
    pymol::Image& dstImage, const Rect2D& srcRect, const Rect2D& dstRect)
{
  auto srcPx = srcImage.pixels();
  auto dstPx = dstImage.pixels() + (dstRect.offset.x * dstRect.extent.width) +
               (dstRect.offset.y * dstRect.extent.height) * srcRect.extent.width;
  int y_limit;
  int x_limit;

  if (((dstRect.offset.y + 1) * dstRect.extent.height) > srcRect.extent.height)
    y_limit =
        srcRect.extent.height - (dstRect.offset.y * dstRect.extent.height);
  else
    y_limit = dstRect.extent.height;
  if (((dstRect.offset.x + 1) * dstRect.extent.width) > srcRect.extent.width)
    x_limit = srcRect.extent.width - (dstRect.offset.x * dstRect.extent.width);
  else
    x_limit = dstRect.extent.width;
  for (int a = 0; a < y_limit; a++) {
    std::copy_n(srcPx, x_limit, dstPx);
    srcPx += srcRect.extent.width;
    dstPx += dstRect.extent.width;
  }
}

UpscaledExtentInfo ExtentGetUpscaleInfo(
    PyMOLGlobals* G, Extent2D extent, const Extent2D& maxExtent, int antialias)
{
  int factor = 0;
  int shift = 0;
  if (antialias == 1) {
    factor = 2;
    shift = 2;
  }
  if (antialias >= 2) {
    factor = 4;
    shift = 4;
  }
  while (factor > 1) {
    if (((extent.width * factor) < maxExtent.width) &&
        ((extent.height * factor) < maxExtent.height)) {
      extent.width *= factor;
      extent.height *= factor;
      break;
    } else {
      factor >>= 1;
      shift -= 2;
      if (factor < 2) {
        G->Feedback->autoAdd(FB_Scene, FB_Blather,
            "Scene-Warning: Maximum OpenGL viewport exceeded. Antialiasing "
            "disabled.");
        break;
      }
    }
  }
  if (factor < 2) {
    factor = 0;
  }

  return UpscaledExtentInfo{
    extent = extent,
    factor = factor,
    shift = shift
  };
}

pymol::Result<> SceneMakeSizedImage(PyMOLGlobals* G, Extent2D extent,
    int antialias, bool excludeSelections, SceneRenderWhich renderWhich)
{
  CScene *I = G->Scene;

  float sceneAspectRatio = SceneGetAspectRatio(G);

  // Calculate from implicit extents if needed
  if (extent.width == 0 && extent.height == 0) {
    extent = SceneGetExtent(G);
  } else if (extent.width != 0 && extent.height == 0) {
    extent.height = static_cast<std::uint32_t>(extent.width / sceneAspectRatio);
  } else if (extent.height != 0 && extent.width == 0) {
    extent.width = static_cast<std::uint32_t>(extent.height * sceneAspectRatio);
  }

  std::optional<Extent2D> saveExtent;
  if (!((extent.width > 0) && (extent.height > 0) && (I->Width > 0) &&
          (I->Height > 0))) {
    if (saveExtent) {
      SceneSetExtent(G, *saveExtent);
    }
    return pymol::make_error(
        "SceneMakeSizedImage-Error: invalid image dimensions");
  }

  if (!(G->HaveGUI && G->ValidContext)) {
    if (saveExtent) {
      SceneSetExtent(G, *saveExtent);
    }
    return {};
  }

  auto maxDim = SceneGLGetMaxDimensions(G);
  auto clampedExtents = ExtentClampByAspectRatio(extent, maxDim);
  auto upscaledExtentInfo =
      ExtentGetUpscaleInfo(G, clampedExtents, maxDim, antialias);
  extent = upscaledExtentInfo.extent;

  if (!saveExtent) {
    saveExtent = SceneGetExtent(G);
  }

  SceneSetExtent(G, extent);
  G->ShaderMgr->bindOffscreenSizedImage(extent, true);

  int nXStep = (extent.width / (I->Width + 1)) + 1;
  int nYStep = (extent.height / (I->Height + 1)) + 1;

  pymol::Image final_image(extent.width, extent.height);

  int total_steps = nXStep * nYStep;

  OrthoBusyPrime(G);

  /* so the trick here is that we need to move the camera around
      so that we get a pixel-perfect mosaic */
  for (int y = 0; y < nYStep; y++) {
    int y_offset = -(I->Height * y);

    for (int x = 0; x < nXStep; x++) {
      int x_offset = -(I->Width * x);

      OrthoBusyFast(G, y * nXStep + x, total_steps);

      auto offscreenFBO = G->ShaderMgr->bindOffscreenSizedImage(extent, true);

      Rect2D viewport{};
      // No offset since this is now done offscreen. In the future,
      // if there's ever a desire to do this in-place, then the
      // offset should be applied.
      viewport.extent = extent;
      SceneRenderInfo renderInfo{};
      renderInfo.mousePos = Offset2D{x_offset, y_offset};
      renderInfo.viewportOverride = viewport;
      renderInfo.excludeSelections = excludeSelections;
      renderInfo.renderWhich = renderWhich;
      renderInfo.offscreenConfig = offscreenFBO;
      G->ShaderMgr->setDrawBuffer(offscreenFBO);

      // JJ: SceneSetViewport in SceneRender will extract the glViewport
      // rather than the Scene extent. Unsure why this is done, so for now
      // just preset the viewport.
      SceneSetViewport(G, viewport);
      SceneRender(G, renderInfo);

      Rect2D srcRect {{}, extent};
      auto image = GLImageToPyMOLImage(G, offscreenFBO, srcRect);

      if (!image.empty()) {
        Rect2D dstRect{{x, y}, SceneGetExtent(G)};
        PyMOLImageCopy(image, final_image, srcRect, dstRect);
      }
    }
  }

  if (!OrthoDeferredWaiting(G)) {
    if (SettingGet<int>(G, cSetting_draw_mode) == -2) {
      ExecutiveSetSettingFromString(
          G, cSetting_draw_mode, "-1", "", -1, true, true);
      SceneUpdate(G, false);
    }
  }

  SceneInvalidateCopy(G, true);

  if (upscaledExtentInfo.factor != 0) { /* are we oversampling? */
    final_image = SceneSupersampleImage(final_image, upscaledExtentInfo);
  }

  ScenePurgeImage(G);

  SceneSetCopyImage(G, std::move(final_image), false, true);

  if (SettingGet<bool>(G, cSetting_opaque_background)) {
    I->Image->m_needs_alpha_reset = true;
  }

  if (saveExtent) {
    SceneSetExtent(G, *saveExtent);
  }

  return {};
}

/**
 * Prepares the scene image for PNG export.
 *
 * If there is no rendered image (`G->Scene->CopyType` is false) then read the
 * current image from the OpenGL back buffer.
 *
 * Sets the alpha channel to opaque if the `opaque_background` setting is on.
 *
 * Modifies `G->Scene->Image`.
 *
 * @param prior_only Return nullptr if there is no prior image (`G->Scene->Image` is nullptr)
 * @return The scene image
 */
pymol::Image* SceneImagePrepare(PyMOLGlobals* G, bool prior_only)
{
  CScene *I = G->Scene;
  pymol::Image* image = nullptr;
  int save_stereo = (I->StereoMode == 1);

  if (!(I->CopyType || prior_only)) {
    if(G->HaveGUI && G->ValidContext) {
      ScenePurgeImage(G);
      I->Image = std::make_shared<pymol::Image>(I->Width, I->Height, save_stereo);
      image = I->Image.get();

#ifndef PURE_OPENGL_ES_2
      if(SceneMustDrawBoth(G) || save_stereo) {
        glReadBuffer(GL_BACK_LEFT);
      } else {
        glReadBuffer(G->ShaderMgr->defaultBackbuffer.drawBuffer); // GL_BACK
      }
#endif
      PyMOLReadPixels(I->rect.left, I->rect.bottom, I->Width, I->Height,
                      GL_RGBA, GL_UNSIGNED_BYTE, (GLvoid *) (image->bits()));
#ifndef PURE_OPENGL_ES_2
      if(save_stereo) {
        glReadBuffer(GL_BACK_RIGHT);
        PyMOLReadPixels(I->rect.left, I->rect.bottom, I->Width, I->Height,
                        GL_RGBA, GL_UNSIGNED_BYTE, (GLvoid *) (image->bits() + image->getSizeInBytes()));
      }
#endif
      I->Image->m_needs_alpha_reset = true;
    }
  } else if(I->Image) {
    image = I->Image.get();
  }
  if(image) {
    int opaque_back = SettingGetGlobal_b(G, cSetting_opaque_background);
    if(opaque_back && I->Image->m_needs_alpha_reset) {
      int i, s = image->getSizeInBytes() * (image->isStereo() ? 2 : 1);
      for (i = pymol::Image::Channel::ALPHA; i < s;
           i += pymol::Image::getPixelSize())
        image->bits()[i] = 0xFF;
      I->Image->m_needs_alpha_reset = false;
    }
  }
  return image;
}

/**
 * Get the size of the rendered image. This is either identical to
 * cmd.get_viewport(), or the dimensions which were last passed to
 * cmd.draw(), cmd.ray() or cmd.png().
 */
std::pair<int, int> SceneGetImageSize(PyMOLGlobals * G)
{
  CScene *I = G->Scene;
  if (I->CopyType && I->Image) {
    return I->Image->getSize();
  } else {
    return std::make_pair(I->Width, I->Height);
  }
}

float SceneGetGridAspectRatio(PyMOLGlobals * G){
  CScene *I = G->Scene;
  auto gridAspRat = (float)(I->grid.cur_viewport_size.width / (float)I->grid.cur_viewport_size.height);
  return SceneGetAspectRatio(G) / gridAspRat;
}

int SceneCopyExternal(PyMOLGlobals * G, int width, int height,
                      int rowbytes, unsigned char *dest, int mode)
{
  auto image = SceneImagePrepare(G, false);
  CScene *I = G->Scene;
  int result = false;
  int i, j;
  int premultiply_alpha = true;
  int red_index = 0, blue_index = 1, green_index = 2, alpha_index = 3;
  int no_alpha = (SettingGetGlobal_b(G, cSetting_opaque_background) &&
                  SettingGetGlobal_b(G, cSetting_ray_opaque_background));

  if(mode & 0x1) {
    int index = 0;
    while(index < 4) {
      if(dest[index] == 'R')
        red_index = index;
      if(dest[index] == 'G')
        green_index = index;
      if(dest[index] == 'B')
        blue_index = index;
      if(dest[index] == 'A')
        alpha_index = index;
      index++;
    }
  }
  if(mode & 0x2) {
    premultiply_alpha = false;
  }
  /*
     printf("image %p I->image %p\n");
     if(I->Image) {
     printf("%d %d %d %d\n",I->Image->width,width,I->Image->height,height);
     } */

  if(image && I->Image && (I->Image->getWidth() == width) && (I->Image->getHeight() == height)) {
    for(i = 0; i < height; i++) {
      unsigned char *src = image->bits() + ((height - 1) - i) * width * 4;
      unsigned char *dst;
      if(mode & 0x4) {
        dst = dest + (height - (i + 1)) * (rowbytes);
      } else {
        dst = dest + i * (rowbytes);
      }
      for(j = 0; j < width; j++) {
        if(no_alpha) {
          dst[red_index] = src[0];      /* no alpha */
          dst[green_index] = src[1];
          dst[blue_index] = src[2];
          dst[alpha_index] = 0xFF;
          /*            if(!(i||j)) {
             printf("no alpha\n");
             } */
        } else if(premultiply_alpha) {
          dst[red_index] = (((unsigned int) src[0]) * src[3]) / 255;    /* premultiply alpha */
          dst[green_index] = (((unsigned int) src[1]) * src[3]) / 255;
          dst[blue_index] = (((unsigned int) src[2]) * src[3]) / 255;
          dst[alpha_index] = src[3];
          /*       if(!(i||j)) {
             printf("premult alpha\n");
             } */
        } else {
          dst[red_index] = src[0];      /* standard alpha */
          dst[green_index] = src[1];
          dst[blue_index] = src[2];
          dst[alpha_index] = src[3];
          /*            if(!(i||j)) {
             printf("standard alpha\n");
             } */
        }
        dst += 4;
        src += 4;
      }
    }
    result = true;
  } else {
    printf("image or size mismatch\n");
  }
  return (result);
}

bool ScenePNG(PyMOLGlobals* G, pymol::zstring_view png, float dpi, int quiet,
    int prior_only, int format, png_outbuf_t* outbuf)
{
  CScene *I = G->Scene;
  SceneImagePrepare(G, prior_only);
  if(I->Image) {
    int width, height;
    std::tie(width, height) = I->Image->getSize();
    auto saveImage = I->Image;
    if(I->Image->isStereo()) {
      saveImage = std::make_shared<pymol::Image>();
      *(saveImage) = I->Image->interlace();
    }
    if(dpi < 0.0F)
      dpi = SettingGetGlobal_f(G, cSetting_image_dots_per_inch);
    auto screen_gamma = SettingGetGlobal_f(G, cSetting_png_screen_gamma);
    auto file_gamma = SettingGetGlobal_f(G, cSetting_png_file_gamma);
    if(MyPNGWrite(png, *saveImage, dpi, format, quiet, screen_gamma, file_gamma, outbuf)) {
      if(!quiet) {
        PRINTFB(G, FB_Scene, FB_Actions)
          " %s: wrote %dx%d pixel image to file \"%s\".\n", __func__,
          width, I->Image->getHeight(), png.c_str() ENDFB(G);
      }
    } else {
      PRINTFB(G, FB_Scene, FB_Errors)
        " %s-Error: error writing \"%s\"! Please check directory...\n", __func__,
        png.c_str() ENDFB(G);
    }
  }
  return I->Image.get() != nullptr;
}


/*========================================================================*/
int SceneGetFrame(PyMOLGlobals * G)
{
  if(MovieDefined(G))
    return (SettingGetGlobal_i(G, cSetting_frame) - 1);
  else
    return (SettingGetGlobal_i(G, cSetting_state) - 1);
}


/*========================================================================*/
/**
 * Returns the number of movie frames, or the number of states if no movie
 * is defined.
 */
int SceneCountFrames(PyMOLGlobals * G)
{
  CScene *I = G->Scene;
  int mov_len = MovieGetLength(G);
  I->HasMovie = (mov_len != 0);
  if(mov_len > 0) {
    I->NFrame = mov_len;
  } else {
    if (mov_len < 0) {
      // allows you to see cached movie even w/o object
      I->NFrame = -mov_len;
    } else {
      I->NFrame = 0;
    }

    for (const auto& obj : I->Obj) {
      int n = obj->getNFrame();
      if (n > I->NFrame)
        I->NFrame = n;
    }
  }
  PRINTFD(G, FB_Scene)" %s: leaving... I->NFrame %d\n", __func__, I->NFrame ENDFD
  return I->NFrame;
}


/*========================================================================*/
void SceneSetFrame(PyMOLGlobals * G, int mode, int frame)
{
  CScene *I = G->Scene;
  int newFrame;
  int newState = 0;
  int movieCommand = false;
  int suppress = false;

  newFrame = SettingGetGlobal_i(G, cSetting_frame) - 1;
  PRINTFD(G, FB_Scene)
    " %s: entered.\n", __func__ ENDFD;
  switch (mode) {
  case -1:                     /* movie/frame override - go to this state absolutely! */
    newState = frame;
    break;
  case 0:                      /* absolute frame */
    newFrame = frame;
    break;
  case 1:                      /* relative frame */
    newFrame += frame;
    break;
  case 2:                      /* end */
    newFrame = I->NFrame - 1;
    break;
  case 3:                      /* middle with automatic movie command */
    newFrame = I->NFrame / 2;
    movieCommand = true;
    break;
  case 4:                      /* absolute with automatic movie command */
    newFrame = frame;
    movieCommand = true;
    break;
  case 5:                      /* relative with automatic movie command */
    newFrame += frame;
    movieCommand = true;
    break;
  case 6:                      /* end with automatic movie command */
    newFrame = I->NFrame - 1;
    movieCommand = true;
    break;
  case 7:                      /* absolute with forced movie command */
    newFrame = frame;
    movieCommand = true;
    break;
  case 8:                      /* relative with forced movie command */
    newFrame += frame;
    movieCommand = true;
    break;
  case 9:                      /* end with forced movie command */
    newFrame = I->NFrame - 1;
    movieCommand = true;
    break;
  case 10:  /* seek forward to current scene (if present) */
    {
      frame = MovieSeekScene(G,true);
      if(frame>=0) {
	newFrame = frame;
	movieCommand = true;
      } else {
	suppress = true;  
      }
    }
    break;
  }
  if(!suppress) {
    SceneCountFrames(G);
    if(mode >= 0) {
      if(newFrame >= I->NFrame)
	newFrame = I->NFrame - 1;
      if(newFrame < 0)
	newFrame = 0;
      newState = MovieFrameToIndex(G, newFrame);
      if(newFrame == 0) {
	if(MovieMatrix(G, cMovieMatrixRecall)) {
	  SceneAbortAnimation(G); /* if we have a programmed initial
				     orientation, don't allow animation
				     to override it */
	}
      }
      SettingSetGlobal_i(G, cSetting_frame, newFrame + 1);
      SettingSetGlobal_i(G, cSetting_state, newState + 1);
      ExecutiveInvalidateSelectionIndicatorsCGO(G);
      SceneInvalidatePicking(G);
      if(movieCommand) {
	MovieDoFrameCommand(G, newFrame);
	MovieFlushCommands(G);
      }
      if(SettingGetGlobal_b(G, cSetting_cache_frames))
	I->MovieFrameFlag = true;
    } else {
      SettingSetGlobal_i(G, cSetting_frame, newFrame + 1);
      SettingSetGlobal_i(G, cSetting_state, newState + 1);
      ExecutiveInvalidateSelectionIndicatorsCGO(G);
      SceneInvalidatePicking(G);
    }
    MovieSetScrollBarFrame(G, newFrame);
    SeqChanged(G); // SceneInvalidate(G);
  }
  PRINTFD(G, FB_Scene)
    " %s: leaving...\n", __func__ ENDFD;
  OrthoInvalidateDoDraw(G);
}


/*========================================================================*/
void SceneDirty(PyMOLGlobals * G)

/* This means that the current image on the screen (and/or in the buffer)

   needs to be updated */
{
  CScene *I = G->Scene;

  PRINTFD(G, FB_Scene)
    " %s: called.\n", __func__ ENDFD;

  if(I) {
    if(!I->DirtyFlag) {
      I->DirtyFlag = true;
      /* SceneInvalidateCopy(G,false); */
      OrthoDirty(G);
    }
  }

}

void SceneRovingPostpone(PyMOLGlobals * G)
{
  CScene *I = G->Scene;
  float delay;
  if(SettingGetGlobal_b(G, cSetting_roving_detail)) {
    delay = SettingGetGlobal_f(G, cSetting_roving_delay);
    if(delay < 0.0F) {
      I->RovingLastUpdate = UtilGetSeconds(G);  /* put off delay */
    }
  }
}

void SceneRovingDirty(PyMOLGlobals * G)
{
  CScene *I = G->Scene;

  if(SettingGetGlobal_b(G, cSetting_roving_detail)) {
    SceneRovingPostpone(G);
    I->RovingDirtyFlag = true;
  }
}


/*========================================================================*/
void SceneChanged(PyMOLGlobals * G)
{
  CScene *I = G->Scene;
  I->ChangedFlag = true;
  SceneInvalidateCopy(G, false);
  SceneDirty(G);
  SeqChanged(G);
  PyMOL_NeedRedisplay(G->PyMOL);
}


/*========================================================================*/
Block *SceneGetBlock(PyMOLGlobals * G)
{
  CScene *I = G->Scene;
  return (I);
}


/*========================================================================*/
int SceneValidateImageMode(PyMOLGlobals * G, int mode, bool defaultdraw) {
  switch (mode) {
    case cSceneImage_Normal:
    case cSceneImage_Draw:
    case cSceneImage_Ray:
      return mode;
  }

  if (mode != cSceneImage_Default) {
    PRINTFB(G, FB_Scene, FB_Warnings)
      " %s-Warning: invalid mode %d\n", __FUNCTION__, mode ENDFB(G);
  }

  if(!G->HaveGUI || SettingGetGlobal_b(G, cSetting_ray_trace_frames)) {
    return cSceneImage_Ray;
  }

  if(defaultdraw || SettingGetGlobal_b(G, cSetting_draw_frames)) {
    return cSceneImage_Draw;
  }

  return cSceneImage_Normal;
}


/*========================================================================*/
int SceneMakeMovieImage(PyMOLGlobals * G,
    int show_timing,
    int validate,
    int mode,
    int width,
    int height)
{
  CScene *I = G->Scene;
  auto requestedExtent = Extent2D{
      static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
  //  float *v;
  int valid = true;
  PRINTFB(G, FB_Scene, FB_Blather)
    " Scene: Making movie image.\n" ENDFB(G);

  // PYMOL-3209 objects inside hidden groups become visible
  ExecutiveUpdateSceneMembers(G);

  mode = SceneValidateImageMode(G, mode, width || height);

  I->DirtyFlag = false;
  switch (mode) {
  case cSceneImage_Ray:
    SceneRay(G, width, height, SettingGetGlobal_i(G, cSetting_ray_default_renderer),
             nullptr, nullptr, 0.0F, 0.0F, false, nullptr, show_timing, -1);
    break;
  case cSceneImage_Draw:
    SceneMakeSizedImage(G, requestedExtent,
        SettingGet<int>(G, cSetting_antialias), /*excludeSelections*/ false);
    break;
  case cSceneImage_Normal:
    {
      auto drawBuffer = SceneDrawBothGetConfig(G);
      if(G->HaveGUI && G->ValidContext) {
        G->ShaderMgr->setDrawBuffer(drawBuffer);
        glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
        /* insert OpenGL context validation code here? */
        SceneRenderInfo renderInfo{};
        SceneRender(G, renderInfo);
        SceneGLClearColor(0.0, 0.0, 0.0, 1.0);
        SceneCopy(G, drawBuffer, true, false);
        /* insert OpenGL context validation code here? */
      }
    }
    break;
  }
  MovieSetImage(G,
                MovieFrameToImage(G, SettingGetGlobal_i(G, cSetting_frame) - 1),
                I->Image);
  if(I->Image)
    I->CopyType = true;
  return valid;
}


/*========================================================================*/
static void SceneUpdateCameraRock(PyMOLGlobals * G, int dirty)
{

  CScene *I = G->Scene;
  float ang_cur, disp, diff;
  float sweep_angle = SettingGetGlobal_f(G, cSetting_sweep_angle);
  float sweep_speed = SettingGetGlobal_f(G, cSetting_sweep_speed);
  float sweep_phase = SettingGetGlobal_f(G, cSetting_sweep_phase);
  int sweep_mode = SettingGetGlobal_i(G, cSetting_sweep_mode);
  float shift = (float) (PI / 2.0F);

  I->SweepTime += I->RenderTime;
  I->LastSweepTime = UtilGetSeconds(G);

  switch (sweep_mode) {
  case 0:
  case 1:
  case 2:
    if(sweep_angle <= 0.0F) {
      diff = (float) ((PI / 180.0F) * I->RenderTime * 10 * sweep_speed / 0.75F);
    } else {
      ang_cur = (float) (I->SweepTime * sweep_speed) + sweep_phase;
      disp = (float) (sweep_angle * (PI / 180.0F) * sin(ang_cur) / 2);
      diff = (float) (disp - I->LastSweep);
      I->LastSweep = disp;
    }
    switch (sweep_mode) {
    case 0:
      SceneRotateWithDirty(G, (float) (180 * diff / PI), 0.0F, 1.0F, 0.0F, dirty);
      break;
    case 1:
      SceneRotateWithDirty(G, (float) (180 * diff / PI), 1.0F, 0.0F, 0.0F, dirty);
      break;
    case 2:                    /* z-rotation...useless! */
      SceneRotateWithDirty(G, (float) (180 * diff / PI), 0.0F, 0.0F, 1.0F, dirty);
      break;
    }
    break;
  case 3:                      /* nutate */
    SceneRotateWithDirty(G, (float) (-I->LastSweepY), 0.0F, 1.0F, 0.0F, dirty);
    SceneRotateWithDirty(G, (float) (-I->LastSweepX), 1.0F, 0.0F, 0.0F, dirty);
    ang_cur = (float) (I->SweepTime * sweep_speed) + sweep_phase;

    I->LastSweepX = (float) (sweep_angle * sin(ang_cur) / 2);
    I->LastSweepY = (float) (sweep_angle * sin(ang_cur + shift) / 2);

    if(I->SweepTime * sweep_speed < PI) {
      float factor = (float) ((I->SweepTime * sweep_speed) / PI);
      I->LastSweepX *= factor;
      I->LastSweepY *= factor;
    }
    SceneRotateWithDirty(G, (float) I->LastSweepX, 1.0F, 0.0F, 0.0F, dirty);
    SceneRotateWithDirty(G, (float) I->LastSweepY, 0.0F, 1.0F, 0.0F, dirty);
    break;
  }
}


/*========================================================================*/
void SceneIdle(PyMOLGlobals * G)
{
  CScene *I = G->Scene;
  double renderTime;
  double minTime;
  int frameFlag = false;

  if(I->PossibleSingleClick == 2) {
    double now = UtilGetSeconds(G);
    double single_click_delay = I->SingleClickDelay;
    double diff = now - I->LastReleaseTime;
    if(diff > single_click_delay) {
      /* post a single click processing event */
      SceneDeferClickWhen(I, I->LastButton + P_GLUT_SINGLE_LEFT, I->LastWinX, I->LastWinY, I->LastClickTime, I->LastMod);        /* push a click onto the queue */

      I->PossibleSingleClick = 0;
      OrthoDirty(G);            /* force an update */
    }
  }
  if(!OrthoDeferredWaiting(G)) {
    if(MoviePlaying(G)) {
      renderTime = UtilGetSeconds(G) - I->LastFrameTime;
      {
        float fps = SettingGetGlobal_f(G, cSetting_movie_fps);
        if(fps <= 0.0F) {
          if(fps < 0.0)
            minTime = 0.0;      /* negative fps means full speed */
          else                  /* 0 fps means use movie_delay instead */
            minTime = SettingGetGlobal_f(G, cSetting_movie_delay) / 1000.0;
          if(minTime >= 0)
            fps = 1.0 / minTime;
          else
            fps = 1000.0F;
        } else {
          minTime = 1.0 / fps;
        }
        if(renderTime >= (minTime - I->LastFrameAdjust)) {
          float adjust = (renderTime - minTime);
          if((fabs(adjust) < minTime) && (fabs(I->LastFrameAdjust) < minTime)) {
            float new_adjust = (renderTime - minTime) + I->LastFrameAdjust;
            I->LastFrameAdjust = (new_adjust + fps * I->LastFrameAdjust) / (1 + fps);
          } else {
            I->LastFrameAdjust = 0.0F;
          }
          frameFlag = true;
        }
      }
    } else if(ControlRocking(G)) {
      renderTime = -I->LastSweepTime + UtilGetSeconds(G);
      minTime = SettingGetGlobal_f(G, cSetting_rock_delay) / 1000.0;
      if(renderTime >= minTime) {
        I->RenderTime = renderTime;
        SceneUpdateCameraRock(G, true);
      }
    }

    if(MoviePlaying(G) && frameFlag) {
      I->LastFrameTime = UtilGetSeconds(G);
      if((SettingGetGlobal_i(G, cSetting_frame) - 1) == (I->NFrame - 1)) {
        if(SettingGetGlobal_b(G, cSetting_movie_loop)) {
          SceneSetFrame(G, 7, 0);
        } else
          MoviePlay(G, cMovieStop);
      } else {
        SceneSetFrame(G, 5, 1);
      }
      PyMOL_NeedRedisplay(G->PyMOL);
    }
  }
}

/*========================================================================*/
/**
 * Zoom to location and radius
 */
void SceneWindowSphere(PyMOLGlobals * G, const float *location, float radius)
{
  CScene *I = G->Scene;
  float v0[3];
  auto pos = I->m_view.pos();

#ifdef _OPENVR_STEREO_DEBUG_VIEWS
  printf("%-20s IP: %11lf %11lf %11lf, IF/IB: %11lf %11lf, IS: %11lf  ==>\n",
    "WindowSphere BEFORE",
    pos.x, pos.y, pos.z,
    I->m_view.m_clip().m_front, I->m_view.m_clip().m_back, I->Scale
  );
#endif // _OPENVR_STEREO_DEBUG_VIEWS

  if (I->StereoMode == cStereo_openvr) {
    I->Scale = 1.0f / radius;
    radius = 1.0;
  } else {
    I->Scale = 1.0;
  }
  float dist = 2.f * radius / GetFovWidth(G);

  /* find where this point is in relationship to the origin */
  subtract3f(glm::value_ptr(I->m_view.origin()), location, v0);

  MatrixTransformC44fAs33f3f(glm::value_ptr(I->m_view.rotMatrix()), v0, glm::value_ptr(pos)); /* convert to view-space */

  if (I->Height > I->Width && I->Height && I->Width)
    dist *= (float)I->Height / (float)I->Width;

#ifdef _PYMOL_OPENVR
  /*lift up molecule to the user's head*/
  if (I->StereoMode == cStereo_openvr) {
    pos.x *= I->Scale;
    pos.y = pos.y * I->Scale + 1.0f; //FIXME make it smart
    pos.z *= I->Scale;
  }
#endif

  pos.z -= dist;
  I->m_view.m_clip().m_front = (-pos.z - radius * 1.2F);
  I->m_view.m_clip().m_back = (-pos.z + radius * 1.2F);
  UpdateFrontBackSafe(I);
  SceneRovingDirty(G);

#ifdef _OPENVR_STEREO_DEBUG_VIEWS
  printf("%-20s IP: %11lf %11lf %11lf, IF/IB: %11lf %11lf, IS: %11lf  ==>\n",
    "WindowSphere AFTER",
    pos.x, pos.y, pos.z,
    I->m_view.m_clip().m_front, I->m_view.m_clip().m_back, I->Scale
  );
#endif // _OPENVR_STEREO_DEBUG_VIEWS
  I->m_view.setPos(pos);
}


/*========================================================================*/
void SceneRelocate(PyMOLGlobals * G, const float *location)
{
  CScene *I = G->Scene;
  float v0[3];
  auto pos = I->m_view.pos();

  auto slab_width = I->m_view.m_clip().m_back - I->m_view.m_clip().m_front;

  /* find out how far camera was from previous origin */
  auto dist = pos.z;

  // stay in front of camera, empirical value to show at least 1 bond
  if (dist > -5.f && I->StereoMode != cStereo_openvr)
    dist = -5.f;

  /* find where this point is in relationship to the origin */
  subtract3f(glm::value_ptr(I->m_view.origin()), location, v0);

  /*  printf("%8.3f %8.3f %8.3f\n",I->m_view.m_clip().m_front,pos.z,I->m_view.m_clip().m_back); */

  auto pos_ptr = glm::value_ptr(pos);
  MatrixTransformC44fAs33f3f(glm::value_ptr(I->m_view.rotMatrix()), v0, pos_ptr); /* convert to view-space */

  pos.z = dist;
  if (I->StereoMode == cStereo_openvr) {
    pos += glm::vec3(0.0f, 1.0f, 0.0f);
  }
  I->m_view.m_clip().m_front = (-pos.z - (slab_width * 0.50F));
  I->m_view.m_clip().m_back = (-pos.z + (slab_width * 0.50F));
  I->m_view.setPos(pos);
  UpdateFrontBackSafe(I);
  SceneRovingDirty(G);

}


/*========================================================================*/
/**
 * Get the origin of rotation in model space
 * cmd.get_view()[12:15]
 *
 * @param[out] origin
 */
void SceneOriginGet(PyMOLGlobals * G, float *origin)
{
  CScene *I = G->Scene;
  copy3f(glm::value_ptr(I->m_view.origin()), origin);
}


/*========================================================================*/
/**
 * Set the origin of rotation in model space
 * (`cmd.get_view()[12:15]`)
 *
 * @param origin New origin
 * @param preserve preserve current viewing location
 */
void SceneOriginSet(PyMOLGlobals * G, const float *origin, int preserve)
{
  CScene *I = G->Scene;
  float v0[3];
  glm::vec3 v1;

  if(preserve) {                /* preserve current viewing location */
    subtract3f(origin, glm::value_ptr(I->m_view.origin()), v0);  /* model-space translation */
    MatrixTransformC44fAs33f3f(glm::value_ptr(I->m_view.rotMatrix()), v0, glm::value_ptr(v1));   /* convert to view-space */
    I->m_view.translate(v1);  /* offset view to compensate */
  }
  I->m_view.setOrigin(origin[0], origin[1], origin[2]); /* move origin */
  SceneInvalidate(G);
}


/*========================================================================*/
int SceneObjectAdd(PyMOLGlobals * G, pymol::CObject * obj)
{
  CScene *I = G->Scene;
  obj->Enabled = true;
  I->Obj.push_back(obj);
  if(obj->type == cObjectGadget) {
    I->GadgetObjs.push_back(obj);
  } else {
    I->NonGadgetObjs.push_back(obj);
  }
  SceneCountFrames(G);
  SceneChanged(G);
  SceneInvalidatePicking(G); // PYMOL-2793
  return 1;
}


/*========================================================================*/
int SceneObjectIsActive(PyMOLGlobals * G, pymol::CObject * obj)
{
  int result = false;
  CScene *I = G->Scene;
  if (find(I->Obj.begin(), I->Obj.end(), obj) != I->Obj.end())
      result = true;
  return result;
}

int SceneObjectDel(PyMOLGlobals * G, pymol::CObject * obj, int allow_purge)
{
  CScene *I = G->Scene;
  int defer_builds_mode = SettingGetGlobal_i(G, cSetting_defer_builds_mode);

  if(!obj) {                    /* deletes all members */
    if(allow_purge && (defer_builds_mode >= 3)) {
      for (auto& obj : I->Obj) {
        /* purge graphics representation when no longer used */
        obj->invalidate(cRepAll, cRepInvPurge, -1);
      }
    }
    I->Obj.clear();
    I->GadgetObjs.clear();
    I->NonGadgetObjs.clear();
  } else {
    auto &obj_list = (obj->type == cObjectGadget) ? I->GadgetObjs : I->NonGadgetObjs;
    auto itg = find(obj_list.begin(), obj_list.end(), obj);
    if (itg != obj_list.end())
      obj_list.erase(itg);

    auto it = find(I->Obj.begin(), I->Obj.end(), obj);
    if (it != I->Obj.end()){
      if(allow_purge && (defer_builds_mode >= 3)) {
        /* purge graphics representation when no longer used */
        (*it)->invalidate(cRepAll, cRepInvPurge, -1);
      }
      obj->Enabled = false;
      I->Obj.erase(it);
    }
  }
  SceneCountFrames(G);
  SceneInvalidate(G);
  SceneInvalidatePicking(G);
  return 0;
}

bool SceneObjectRemove(PyMOLGlobals* G, pymol::CObject* obj)
{
  if (obj == nullptr) {
    return true;
  }
  CScene* I = G->Scene;
  auto& obj_list =
      (obj->type == cObjectGadget) ? I->GadgetObjs : I->NonGadgetObjs;
  auto it = std::find(obj_list.begin(), obj_list.end(), obj);
  if(it == obj_list.end()){
    return false;
  }
  obj_list.erase(it, obj_list.end());
  return true;
}

/*========================================================================*/
int SceneLoadPNG(PyMOLGlobals * G, const char *fname, int movie_flag, int stereo, int quiet)
{
  CScene *I = G->Scene;
  int ok = false;
  if(I->Image) {
    ScenePurgeImage(G);
    I->CopyType = false;
    OrthoInvalidateDoDraw(G); // right now, need to invalidate since text could be shown
  }
  I->Image = MyPNGRead(fname);
  if(I->Image) {
    if(!quiet) {
      PRINTFB(G, FB_Scene, FB_Details)
        " Scene: loaded image from '%s'.\n", fname ENDFB(G);
    }
    if((stereo > 0) || ((stereo < 0) &&
                        (I->Image->getWidth() == 2 * I->Width) &&
                        (I->Image->getHeight() == I->Height))) {
      *(I->Image) = I->Image->deinterlace(stereo == 2);
    }

    I->CopyType = true;
    I->CopyForced = true;
    OrthoRemoveSplash(G);
    SettingSetGlobal_b(G, cSetting_text, 0);
    if(movie_flag &&
       I->Image && !I->Image->empty()) {
      MovieSetImage(G, MovieFrameToImage(G, SettingGetGlobal_i(G, cSetting_frame) - 1)
                    , I->Image);
      I->MovieFrameFlag = true;
    } else {
      I->DirtyFlag = false;     /* make sure we don't overwrite image */
    }
    OrthoDirty(G);
    ok = true;
  } else {
    if(!quiet) {
      PRINTFB(G, FB_Scene, FB_Errors)
        " Scene: unable to load image from '%s'.\n", fname ENDFB(G);
    }
  }
  return (ok);
}


/*static unsigned int byte_max(unsigned int value)
{
  return (value>0xFF) ? 0xFF : value;
}
*/

#define SceneClickMargin DIP2PIXEL(2)
#define SceneTopMargin 0
#define SceneToggleMargin DIP2PIXEL(2)
#define SceneRightMargin 0
#define SceneToggleWidth DIP2PIXEL(17)
#define SceneToggleSize DIP2PIXEL(16)
#define SceneToggleTextShift DIP2PIXEL(4)
#define SceneTextLeftMargin DIP2PIXEL(1)
#ifndef _PYMOL_NOPY
static void draw_button(int x2, int y2, int z, int w, int h, float *light, float *dark,
                        float *inside , CGO *orthoCGO)
{
  if (orthoCGO){
    CGOColorv(orthoCGO, light);
    CGOBegin(orthoCGO, GL_TRIANGLE_STRIP);
    CGOVertex(orthoCGO, x2, y2, z);
    CGOVertex(orthoCGO, x2, y2 + h, z);
    CGOVertex(orthoCGO, x2 + w, y2, z);
    CGOVertex(orthoCGO, x2 + w, y2 + h, z);
    CGOEnd(orthoCGO);
  } else {
    glColor3fv(light);
    glBegin(GL_POLYGON);
    glVertex3i(x2, y2, z);
    glVertex3i(x2, y2 + h, z);
    glVertex3i(x2 + w, y2 + h, z);
    glVertex3i(x2 + w, y2, z);
    glEnd();
  }

  if (orthoCGO){
    CGOColorv(orthoCGO, dark);
    CGOBegin(orthoCGO, GL_TRIANGLE_STRIP);
    CGOVertex(orthoCGO, x2 + 1, y2, z);
    CGOVertex(orthoCGO, x2 + 1, y2 + h - 1, z);
    CGOVertex(orthoCGO, x2 + w, y2, z);
    CGOVertex(orthoCGO, x2 + w, y2 + h - 1, z);
    CGOEnd(orthoCGO);
  } else {
    glColor3fv(dark);
    glBegin(GL_POLYGON);
    glVertex3i(x2 + 1, y2, z);
    glVertex3i(x2 + 1, y2 + h - 1, z);
    glVertex3i(x2 + w, y2 + h - 1, z);
    glVertex3i(x2 + w, y2, z);
    glEnd();
  }

  if(inside) {
    if (orthoCGO){
      CGOColorv(orthoCGO, inside);
      CGOBegin(orthoCGO, GL_TRIANGLE_STRIP);
      CGOVertex(orthoCGO, x2 + 1, y2 + 1, z);
      CGOVertex(orthoCGO, x2 + 1, y2 + h - 1, z);
      CGOVertex(orthoCGO, x2 + w - 1, y2 + 1, z);
      CGOVertex(orthoCGO, x2 + w - 1, y2 + h - 1, z);
      CGOEnd(orthoCGO);
    } else {
      glColor3fv(inside);
      glBegin(GL_POLYGON);
      glVertex3i(x2 + 1, y2 + 1, z);
      glVertex3i(x2 + 1, y2 + h - 1, z);
      glVertex3i(x2 + w - 1, y2 + h - 1, z);
      glVertex3i(x2 + w - 1, y2 + 1, z);
      glEnd();
    }
  } else {                      /* rainbow */
    if (orthoCGO){
      CGOBegin(orthoCGO, GL_TRIANGLE_STRIP);
      CGOColor(orthoCGO, 0.1F, 1.0F, 0.1F); // green
      CGOVertex(orthoCGO, x2 + 1, y2 + h - 1, z);
      CGOColor(orthoCGO, 1.0F, 1.0F, 0.1F);  // yellow
      CGOVertex(orthoCGO, x2 + w - 1, y2 + h - 1, z);
      CGOColor(orthoCGO, 1.f, 0.1f, 0.1f); // red
      CGOVertex(orthoCGO, x2 + 1, y2 + 1, z);
      CGOColor(orthoCGO, 0.1F, 0.1F, 1.0F);  // blue
      CGOVertex(orthoCGO, x2 + w - 1, y2 + 1, z);
      CGOEnd(orthoCGO);
    } else {
      glBegin(GL_POLYGON);
      glColor3f(1.0F, 0.1F, 0.1F);
      glVertex3i(x2 + 1, y2 + 1, z);
      glColor3f(0.1F, 1.0F, 0.1F);
      glVertex3i(x2 + 1, y2 + h - 1, z);
      glColor3f(1.0F, 1.0F, 0.1F);
      glVertex3i(x2 + w - 1, y2 + h - 1, z);
      glColor3f(0.1F, 0.1F, 1.0F);
      glVertex3i(x2 + w - 1, y2 + 1, z);
      glEnd();
    }
  }
}
#endif

/**
 * Update the G->Scene->SceneVLA names array which is used for scene buttons
 *
 * @param list List of scene names
 */
void SceneSetNames(PyMOLGlobals * G, const std::vector<std::string> &list)
{
  CScene *I = G->Scene;
  I->SceneVec.clear();
  I->SceneVec.reserve(list.size());

  for (const auto& name : list) {
    I->SceneVec.emplace_back(name, false);
  }

  OrthoDirty(G);
}


/*========================================================================*/
static void SceneDrawButtons(Block * block, int draw_for_real , CGO *orthoCGO)
{
#ifndef _PYMOL_NOPY
  PyMOLGlobals *G = block->m_G;
  CScene *I = G->Scene;
  int x, y, xx, x2;
  float enabledColor[3] = { 0.5F, 0.5F, 0.5F };
  float pressedColor[3] = { 0.7F, 0.7F, 0.7F };
  float disabledColor[3] = { 0.25F, 0.25F, 0.25F };
  float lightEdge[3] = { 0.6F, 0.6F, 0.6F };
  float darkEdge[3] = { 0.35F, 0.35F, 0.35F };
  int charWidth = DIP2PIXEL(8);
  int n_ent;
  int n_disp;
  int skip = 0;
  int row = -1;
  int lineHeight = DIP2PIXEL(SettingGetGlobal_i(G, cSetting_internal_gui_control_size));
  int text_lift = (lineHeight / 2) - DIP2PIXEL(5);
  int op_cnt = 1;

  if(((G->HaveGUI && G->ValidContext) || (!draw_for_real)) &&
     ((block->rect.right - block->rect.left) > 6) && (!I->SceneVec.empty())) {
    int max_char;
    int nChar;
    I->ButtonsShown = true;

    /* do we have enough structures to warrant a scroll bar? */
    n_ent = I->SceneVec.size();

    n_disp =
      (((I->rect.top - I->rect.bottom) - (SceneTopMargin)) / lineHeight) -
      1;
    if(n_disp < 1)
      n_disp = 1;

    {
      for (auto& elem : I->SceneVec)
        elem.drawn = false;
    }
    if(n_ent > n_disp) {
      int bar_maxed = I->m_ScrollBar.isMaxed();
      if(!I->ScrollBarActive) {
        I->m_ScrollBar.setLimits(n_ent, n_disp);
        if(bar_maxed) {
          I->m_ScrollBar.maxOut();
          I->NSkip = static_cast<int>(I->m_ScrollBar.getValue());
        } else {
          I->m_ScrollBar.setValue(0);
          I->NSkip = 0;
        }
      } else {
        I->m_ScrollBar.setLimits(n_ent, n_disp);
        if(bar_maxed)
          I->m_ScrollBar.maxOut();
        I->NSkip = static_cast<int>(I->m_ScrollBar.getValue());
      }
      I->ScrollBarActive = 1;

    } else {
      I->ScrollBarActive = 0;
      I->NSkip = 0;
    }

    max_char = (((I->rect.right - I->rect.left) -
                 (SceneTextLeftMargin + SceneRightMargin + 4)) -
                (op_cnt * SceneToggleWidth));
    if(I->ScrollBarActive) {
      max_char -= (SceneScrollBarMargin + SceneScrollBarWidth);
    }
    max_char /= charWidth;

    if(I->ScrollBarActive) {
      I->m_ScrollBar.setBox(I->rect.top - SceneScrollBarMargin,
                            I->rect.left + SceneScrollBarMargin,
                            I->rect.bottom + 2,
                            I->rect.left + SceneScrollBarMargin + SceneScrollBarWidth);
      if(draw_for_real)
        I->m_ScrollBar.draw(orthoCGO);
    }

    skip = I->NSkip;
    x = I->rect.left + SceneTextLeftMargin;

    /*    y = ((I->rect.top-lineHeight)-SceneTopMargin)-lineHeight; */

    {
      int n_vis = n_disp;
      if(n_ent < n_vis)
        n_vis = n_ent;
      y = (I->rect.bottom + SceneBottomMargin) + (n_vis - 1) * lineHeight;
    }

    /*    xx = I->rect.right-SceneRightMargin-SceneToggleWidth*(cRepCnt+op_cnt); */
    xx = I->rect.right - SceneRightMargin - SceneToggleWidth * (op_cnt);

    if(I->ScrollBarActive) {
      x += SceneScrollBarWidth + SceneScrollBarMargin;
    }
    {
      int i;

      for(i = 0; i < n_ent; i++) {
        if(skip) {
          skip--;
        } else {
          row++;
          x2 = xx;
          nChar = max_char;

          if((x - SceneToggleMargin) - (xx - SceneToggleMargin) > -10) {
            x2 = x + 10;
          }
          {
            float toggleColor[3] = { 0.5F, 0.5F, 1.0F };

            if(draw_for_real) {
              glColor3fv(toggleColor);

              TextSetColor(G, I->TextColor);
              TextSetPos2i(G, x + DIP2PIXEL(2), y + text_lift);
            }
            {
              const char *cur_name = SettingGetGlobal_s(G, cSetting_scene_current_name);
              auto elem = &I->SceneVec[i];
              int item = I->NSkip + row;
              auto c = elem->name.c_str();
              int len = static_cast<int>(elem->name.size());

              x2 = xx;
              if(len > max_char)
                len = max_char;
              x2 = x + len * charWidth + DIP2PIXEL(6);

              /* store rectangles for finding clicks */

              elem->drawn = true;

              elem->rect = pymol::Rect<int>(x, x2, y, y + lineHeight);

              if(I->ButtonMargin < x2)
                I->ButtonMargin = x2;

              if(draw_for_real) {

                if((item == I->Pressed) && (item == I->Over)) {
                  draw_button(x, y, 0, (x2 - x) - 1, (lineHeight - 1), lightEdge,
                              darkEdge, pressedColor, orthoCGO);
                } else if(cur_name && elem->name == cur_name) {
                  draw_button(x, y, 0, (x2 - x) - 1, (lineHeight - 1), lightEdge,
                              darkEdge, enabledColor, orthoCGO);
                } else {
                  draw_button(x, y, 0, (x2 - x) - 1, (lineHeight - 1), lightEdge,
                              darkEdge, disabledColor, orthoCGO);
                }

                TextSetColor(G, I->TextColor);

                if(c) {
                  while(*c) {
                    if((nChar--) > 0)
                      TextDrawChar(G, *(c++), orthoCGO);
                    else
                      break;
                  }
                }
              }
            }
          }
          y -= lineHeight;
          if(y < (I->rect.bottom))
            break;
        }
      }
    }
    I->HowFarDown = y;
    I->ButtonsValid = true;
  }
#endif
}

// TODO: Replace with ShaderMgr::drawPixelsTo
static void RendererWritePixelsTo(
    PyMOLGlobals* G, const Rect2D& rect, unsigned char* buffer)
{
#ifndef PURE_OPENGL_ES_2
  glRasterPos3i(rect.offset.x, rect.offset.y, -10);
#endif
  PyMOLDrawPixels(rect.extent.width, rect.extent.height, GL_RGBA, GL_UNSIGNED_BYTE, buffer);
}

static bool SceneOverlayOversize(
    PyMOLGlobals* G, unsigned char* data, CGO* orthoCGO)
{
  bool drawn = false;
  auto I = G->Scene;
  auto show_alpha = SettingGet<bool>(G, cSetting_show_alpha_checker);
  const float* bg_color =
      ColorGet(G, SettingGet<int>(G, nullptr, nullptr, cSetting_bg_rgb));
  unsigned int bg_rr, bg_r = (unsigned int) (255 * bg_color[0]);
  unsigned int bg_gg, bg_g = (unsigned int) (255 * bg_color[1]);
  unsigned int bg_bb, bg_b = (unsigned int) (255 * bg_color[2]);

  int factor = 1;
  int shift = 0;
  int tmp_height = I->Image->getHeight();
  int tmp_width = I->Image->getWidth();
  int src_row_bytes = I->Image->getWidth() * pymol::Image::getPixelSize();
  unsigned int color_word;
  float rgba[4] = {0.0F, 0.0F, 0.0F, 1.0F};

  ColorGetBkrdContColor(G, rgba, false);
  color_word = ColorGet32BitWord(G, rgba);

  while (tmp_height && tmp_width &&
         ((tmp_height > (I->Height - 3)) || (tmp_width > (I->Width - 3)))) {
    tmp_height = (tmp_height >> 1);
    tmp_width = (tmp_width >> 1);
    factor = (factor << 1);
    shift++;
  }
  tmp_width += 2;
  tmp_height += 2;

  if (tmp_height && tmp_width) {
    unsigned int buffer_size = tmp_height * tmp_width * 4;
    std::vector<unsigned char> buffer_vec(buffer_size);
    auto* buffer = buffer_vec.data();

    if (!buffer_vec.empty() && data) {
      unsigned char* p = data;
      unsigned char* q = buffer;
      unsigned char *pp, *ppp, *pppp;
      unsigned int c1, c2, c3, c4, tot, bg;
      unsigned int factor_col_bytes = factor * 4;
      unsigned int factor_row_bytes = factor * src_row_bytes;

      shift = shift + shift;

      for (int a = 0; a < tmp_width; a++) { /* border, first row */
        *((unsigned int*) (q)) = color_word;
        q += 4;
      }
      for (int b = 1; b < tmp_height - 1; b++) { /* rows */
        pp = p;
        *((unsigned int*) (q)) = color_word; /* border */
        q += 4;
        for (int a = 1; a < tmp_width - 1; a++) { /* cols */
          ppp = pp;

          c1 = c2 = c3 = c4 = tot = 0;

          if (show_alpha &&
              (((a >> 4) + (b >> 4)) & 0x1)) { /* introduce checkerboard */
            bg_rr = ((bg_r & 0x80) ? bg_r - TRN_BKG : bg_r + TRN_BKG);
            bg_gg = ((bg_g & 0x80) ? bg_g - TRN_BKG : bg_g + TRN_BKG);
            bg_bb = ((bg_b & 0x80) ? bg_b - TRN_BKG : bg_b + TRN_BKG);
          } else {
            bg_rr = bg_r;
            bg_gg = bg_g;
            bg_bb = bg_b;
          }

          for (int d = 0; d < factor; d++) { /* box rows */
            pppp = ppp;
            for (int c = 0; c < factor; c++) { /* box cols */
              unsigned char alpha = pppp[3];
              c1 += *(pppp++) * alpha;
              c2 += *(pppp++) * alpha;
              c3 += *(pppp++) * alpha;
              pppp++;
              c4 += alpha;
              tot += 0xFF;
            }
            ppp += src_row_bytes;
          }
          if (c4) {
            bg = tot - c4;
            *(q++) = (c1 + bg_rr * bg) / tot;
            *(q++) = (c2 + bg_gg * bg) / tot;
            *(q++) = (c3 + bg_bb * bg) / tot;
            *(q++) = 0xFF;
          } else {
            *(q++) = bg_rr;
            *(q++) = bg_gg;
            *(q++) = bg_bb;
            *(q++) = 0xFF;
          }
          pp += factor_col_bytes;
        }
        *((unsigned int*) (q)) = color_word; /* border */
        q += 4;
        p += factor_row_bytes;
      }
      for (int a = 0; a < tmp_width; a++) { /* border, last row */
        *((unsigned int*) (q)) = color_word;
        q += 4;
      }
      Rect2D rect{{(int) ((I->Width - tmp_width) / 2 + I->rect.left),
                      (int) ((I->Height - tmp_height) / 2 + I->rect.bottom)},
          {tmp_width, tmp_height}};
      RendererWritePixelsTo(G, rect, buffer);
      drawn = true;
    }
  }
  int text_pos = (I->Height - tmp_height) / 2 - 15;
  int x_pos, y_pos;
  if (text_pos < 0) {
    text_pos = (I->Height - tmp_height) / 2 + 3;
    x_pos = (I->Width - tmp_width) / 2 + 3;
    y_pos = text_pos;
  } else {
    x_pos = (I->Width - tmp_width) / 2;
    y_pos = text_pos;
  }

  auto buffer = pymol::join_to_string(
      "Image size = ", I->Image->getWidth(), " x ", I->Image->getHeight());

  TextSetColor3f(G, rgba[0], rgba[1], rgba[2]);
  TextDrawStrAt(G, buffer.c_str(), x_pos + I->rect.left,
      y_pos + I->rect.bottom, orthoCGO);
  return drawn;
}

static bool SceneOverlayOversizeBorder(
    PyMOLGlobals* G, int width, int height, unsigned char* data)
{
  /* but a border around image */
  auto I = G->Scene;
  bool drawn = false;

  auto show_alpha = SettingGet<bool>(G, cSetting_show_alpha_checker);
  const float* bg_color =
      ColorGet(G, SettingGet<int>(G, nullptr, nullptr, cSetting_bg_rgb));
  unsigned int bg_rr, bg_r = (unsigned int) (255 * bg_color[0]);
  unsigned int bg_gg, bg_g = (unsigned int) (255 * bg_color[1]);
  unsigned int bg_bb, bg_b = (unsigned int) (255 * bg_color[2]);

  unsigned int color_word;
  float rgba[4] = {0.0F, 0.0F, 0.0F, 1.0F};
  unsigned int tmp_height = height + 2;
  unsigned int tmp_width = width + 2;
  unsigned int border = 1;
  unsigned int upscale = 1;

  // Upscale for Retina/4K
  if (DIP2PIXEL(height) == I->Height && DIP2PIXEL(width) == I->Width) {
    upscale = DIP2PIXEL(1);
    tmp_height = DIP2PIXEL(height);
    tmp_width = DIP2PIXEL(width);
    border = 0;
  }

  unsigned int n_word = tmp_height * tmp_width;
  std::vector<unsigned int> tmp_buffer_vec(n_word);
  ColorGetBkrdContColor(G, rgba, false);
  color_word = ColorGet32BitWord(G, rgba);

  if (!tmp_buffer_vec.empty()) {
    auto* tmp_buffer = tmp_buffer_vec.data();
    unsigned int a, b;
    unsigned int* p = (unsigned int*) data;
    unsigned int* q = tmp_buffer;

    // top border
    for (a = 0; a < border; ++a) {
      for (b = 0; b < tmp_width; b++)
        *(q++) = color_word;
    }

    for (a = border; a < tmp_height - border; a++) {
      // left border
      for (b = 0; b < border; ++b) {
        *(q++) = color_word;
      }

      for (b = border; b < tmp_width - border; b++) {
        unsigned char* qq = (unsigned char*) q;
        unsigned char* pp = (unsigned char*) p;
        unsigned char bg;
        if (show_alpha &&
            (((a >> 4) + (b >> 4)) & 0x1)) { /* introduce checkerboard */
          bg_rr = ((bg_r & 0x80) ? bg_r - TRN_BKG : bg_r + TRN_BKG);
          bg_gg = ((bg_g & 0x80) ? bg_g - TRN_BKG : bg_g + TRN_BKG);
          bg_bb = ((bg_b & 0x80) ? bg_b - TRN_BKG : bg_b + TRN_BKG);
        } else {
          bg_rr = bg_r;
          bg_gg = bg_g;
          bg_bb = bg_b;
        }
        if (pp[3]) {
          bg = 0xFF - pp[3];
          *(qq++) = (pp[0] * pp[3] + bg_rr * bg) / 0xFF;
          *(qq++) = (pp[1] * pp[3] + bg_gg * bg) / 0xFF;
          *(qq++) = (pp[2] * pp[3] + bg_bb * bg) / 0xFF;
          *(qq++) = 0xFF;
        } else {
          *(qq++) = bg_rr;
          *(qq++) = bg_gg;
          *(qq++) = bg_bb;
          *(qq++) = 0xFF;
        }
        q++;

        if ((b + 1 - border) % upscale == 0) {
          p++;
        }
      }

      if ((a + 1 - border) % upscale != 0) {
        // read row again
        p -= width;
      }

      // right border
      for (b = 0; b < border; ++b) {
        *(q++) = color_word;
      }
    }

    // bottom border
    for (a = 0; a < border; ++a) {
      for (b = 0; b < tmp_width; b++)
        *(q++) = color_word;
    }

    Rect2D rect{{(int) ((I->Width - tmp_width) / 2 + I->rect.left),
                    (int) ((I->Height - tmp_height) / 2 + I->rect.bottom)},
        {tmp_width, tmp_height}};
    RendererWritePixelsTo(G, rect, (unsigned char*) tmp_buffer);
    drawn = true;
  }
  return drawn;
}

static bool SceneOverlayExactFitNoAlpha(PyMOLGlobals* G, int width, int height, unsigned char* data)
{
  auto I = G->Scene;
  Rect2D rect{{(int) ((I->Width - width) / 2 + I->rect.left),
                  (int) ((I->Height - height) / 2 + I->rect.bottom)},
      {width, height}};
  RendererWritePixelsTo(G, rect, data);
  return true;
}

static bool SceneOverlayExactFit(PyMOLGlobals* G, int width, int height, unsigned char* data)
{
  auto I = G->Scene;
  float rgba[4] = {0.0F, 0.0F, 0.0F, 1.0F};
  unsigned int n_word = height * width;
  std::vector<unsigned int> tmp_buffer_vec(n_word);
  ColorGetBkrdContColor(G, rgba, false);

  auto show_alpha = SettingGet<bool>(G, cSetting_show_alpha_checker);
  const float* bg_color =
      ColorGet(G, SettingGet<int>(G, nullptr, nullptr, cSetting_bg_rgb));
  unsigned int bg_rr, bg_r = (unsigned int) (255 * bg_color[0]);
  unsigned int bg_gg, bg_g = (unsigned int) (255 * bg_color[1]);
  unsigned int bg_bb, bg_b = (unsigned int) (255 * bg_color[2]);

  if (tmp_buffer_vec.empty()) {
    return false;
  }
  auto* tmp_buffer = tmp_buffer_vec.data();
  unsigned int a, b;
  unsigned int* p = (unsigned int*) data;
  unsigned int* q = tmp_buffer;
  for (a = 0; a < (unsigned int) height; a++) {
    for (b = 0; b < (unsigned int) width; b++) {
      unsigned char* qq = (unsigned char*) q;
      unsigned char* pp = (unsigned char*) p;
      unsigned char bg;
      if (show_alpha &&
          (((a >> 4) + (b >> 4)) & 0x1)) { /* introduce checkerboard */
        bg_rr = ((bg_r & 0x80) ? bg_r - TRN_BKG : bg_r + TRN_BKG);
        bg_gg = ((bg_g & 0x80) ? bg_g - TRN_BKG : bg_g + TRN_BKG);
        bg_bb = ((bg_b & 0x80) ? bg_b - TRN_BKG : bg_b + TRN_BKG);
      } else {
        bg_rr = bg_r;
        bg_gg = bg_g;
        bg_bb = bg_b;
      }
      if (pp[3]) {
        bg = 0xFF - pp[3];
        *(qq++) = (pp[0] * pp[3] + bg_rr * bg) / 0xFF;
        *(qq++) = (pp[1] * pp[3] + bg_gg * bg) / 0xFF;
        *(qq++) = (pp[2] * pp[3] + bg_bb * bg) / 0xFF;
        *(qq++) = 0xFF;
      } else {
        *(qq++) = bg_rr;
        *(qq++) = bg_gg;
        *(qq++) = bg_bb;
        *(qq++) = 0xFF;
      }
      q++;
      p++;
    }
  }
  Rect2D rect{{(int) ((I->Width - width) / 2 + I->rect.left),
                  (int) ((I->Height - height) / 2 + I->rect.bottom)},
      {width, height}};
  RendererWritePixelsTo(G, rect, (unsigned char*) tmp_buffer);
  return true;
}

int SceneDrawImageOverlay(PyMOLGlobals* G, int override, CGO* orthoCGO)
{
  CScene* I = G->Scene;
  int drawn = false;
  int text = SettingGet<bool>(G, cSetting_text);
  /* is the text/overlay (ESC) on? */
  int overlay = OrthoGetOverlayStatus(G);
  bool draw_overlay = (!text || overlay) && (override || I->CopyType == true) &&
                      I->Image && !I->Image->empty();

  if (!draw_overlay) {
    return drawn;
  }

  int width = I->Image->getWidth();
  int height = I->Image->getHeight();
  unsigned char* data = I->Image->bits();

#ifndef PURE_OPENGL_ES_2
  if (I->Image->isStereo()) {
    int buffer;
    glGetIntegerv(GL_DRAW_BUFFER, (GLint*) &buffer);
    if (buffer == GL_BACK_RIGHT) /* hardware stereo */
      data += I->Image->getSizeInBytes();
    else {
      int stereo = SettingGetGlobal_i(G, cSetting_stereo);
      if (stereo) {
        switch (OrthoGetRenderMode(G)) {
        case OrthoRenderMode::GeoWallRight:
          data += I->Image->getSizeInBytes();
          break;
        default:
          break;
        }
      }
    }
  }
#endif

  if ((height > I->Height) || (width > I->Width)) { /* image is oversize */
    drawn = SceneOverlayOversize(G, data, orthoCGO);
  } else if (((width < I->Width) || (height < I->Height)) &&
              ((I->Width - width) > 2) && ((I->Height - height) > 2)) {
    drawn = SceneOverlayOversizeBorder(G, width, height, data);
  } else if (I->CopyForced) { /* near-exact fit */
    drawn = SceneOverlayExactFit(G, width, height, data);
  } else { /* not a forced copy, so don't show/blend alpha */
    drawn = SceneOverlayExactFitNoAlpha(G, width, height, data);
  }

  I->LastRender = UtilGetSeconds(G);
  return drawn;
}

void CScene::draw(CGO* orthoCGO) /* returns true if scene was drawn (using a cached image) */
{
  PyMOLGlobals *G = m_G;
  CScene *I = G->Scene;
  int drawn = false; 

  if(G->HaveGUI && G->ValidContext) {

    I->ButtonsShown = false;

    drawn = SceneDrawImageOverlay(G, 0, orthoCGO);

    if(SettingGetGlobal_b(G, cSetting_scene_buttons)) {
      SceneDrawButtons(this, true, orthoCGO);
    } else {
      I->ButtonMargin = 0;
    }
  }
  if(drawn)
    OrthoDrawWizardPrompt(G, orthoCGO); /* ugly hack necessitated because wizard
						prompt is overwritten when image is drawn */

}

/*========================================================================*/
void SceneDoRoving(PyMOLGlobals * G, float old_front,
                          float old_back, float old_origin,
                          int adjust_flag, int zoom_flag)
{
  EditorFavorOrigin(G, nullptr);
  if(SettingGetGlobal_b(G, cSetting_roving_origin)) {

    CScene *I = G->Scene;
    float delta_front, delta_back;
    float front_weight, back_weight, slab_width;
    float z_buffer = 3.0;
    float old_pos2 = 0.0F;
    float v2[3];

    z_buffer = SettingGetGlobal_f(G, cSetting_roving_origin_z_cushion);

    delta_front = I->m_view.m_clip().m_front - old_front;
    delta_back = I->m_view.m_clip().m_back - old_back;

    zero3f(v2);

    slab_width = I->m_view.m_clip().m_back - I->m_view.m_clip().m_front;

    /* first, check to make sure that the origin isn't too close to either plane */
    if((z_buffer * 2) > slab_width)
      z_buffer = slab_width * 0.5F;

    if(old_origin < (I->m_view.m_clip().m_front + z_buffer)) {    /* old origin behind front plane */
      front_weight = 1.0F;
      delta_front = (I->m_view.m_clip().m_front + z_buffer) - old_origin; /* move origin into allowed regioin */
    } else if(old_origin > (I->m_view.m_clip().m_back - z_buffer)) {      /* old origin was behind back plane */
      front_weight = 0.0F;
      delta_back = (I->m_view.m_clip().m_back - z_buffer) - old_origin;

    } else if(slab_width >= R_SMALL4) { /* otherwise, if slab exists */
      front_weight = (old_back - old_origin) / slab_width;      /* weight based on relative proximity */
    } else {
      front_weight = 0.5F;
    }

    back_weight = 1.0F - front_weight;

    if((front_weight > 0.2) && (back_weight > 0.2)) {   /* origin not near edge */
      if(delta_front * delta_back > 0.0F) {     /* planes moving in same direction */
        if(fabs(delta_front) > fabs(delta_back)) {      /* so stick with whichever moves less */
          v2[2] = delta_back;
        } else {
          v2[2] = delta_front;
        }
      } else {
        /* planes moving in opposite directions (increasing slab size) */
        /* don't move origin */
      }
    } else {                    /* origin is near edge -- move origin with plane having highest weight */
      if(front_weight < back_weight) {
        v2[2] = delta_back;
      } else {
        v2[2] = delta_front;
      }
    }

    old_pos2 = I->m_view.pos().z;

    MatrixInvTransformC44fAs33f3f(glm::value_ptr(I->m_view.rotMatrix()), v2, v2);        /* transform offset into realspace */
    subtract3f(glm::value_ptr(I->m_view.origin()), v2, v2);      /* calculate new origin location */
    SceneOriginSet(G, v2, true);        /* move origin, preserving camera location */

    if(SettingGetGlobal_b(G, cSetting_ortho) || zoom_flag) {
      /* we're orthoscopic, so we don't want the effective field of view 
         to change.  Thus, we have to hold Pos[2] constant, and instead
         move the planes.
       */
      float delta = old_pos2 - I->m_view.pos().z;
      I->m_view.translate(0, 0, delta);
      SceneClipSet(G, I->m_view.m_clip().m_front - delta, I->m_view.m_clip().m_back - delta);
    }
    slab_width = I->m_view.m_clip().m_back - I->m_view.m_clip().m_front;

    /* first, check to make sure that the origin isn't too close to either plane */
    if((z_buffer * 2) > slab_width)
      z_buffer = slab_width * 0.5F;

  }
  if((adjust_flag) && SettingGetGlobal_b(G, cSetting_roving_detail)) {
    SceneRovingPostpone(G);
  }
  if(SettingGetGlobal_b(G, cSetting_roving_detail)) {
    SceneRovingDirty(G);
  }
}

float ScenePushRasterMatrix(PyMOLGlobals * G, float *v)
{
  float scale = SceneGetExactScreenVertexScale(G, v);
#ifndef PURE_OPENGL_ES_2
  CScene *I = G->Scene;
  glMatrixMode(GL_MODELVIEW);
  glPushMatrix();
  glTranslatef(v[0], v[1], v[2]);       /* go to this position */
  glMultMatrixf(I->InvMatrix);
  glScalef(scale, scale, scale);
#endif
  return scale;
}

void ScenePopRasterMatrix(PyMOLGlobals * G)
{
#ifdef PURE_OPENGL_ES_2
    /* TODO */
#else
  glMatrixMode(GL_MODELVIEW);
  glPopMatrix();
#endif
}

/**
 * Compose the ModelViewMatrix from Pos, RotMatrix and Origin
 * See also: CScene.ModMatrix (queried from OpenGL)
 *
 * @param[out] modelView 4x4 matrix
 */
static void SceneComposeModelViewMatrix(CScene * I, float * modelView) {
  identity44f(modelView);
  const auto& pos = I->m_view.pos();
  const auto& ori = I->m_view.origin();
  MatrixTranslateC44f(modelView, pos.x, pos.y, pos.z);
  MatrixMultiplyC44f(glm::value_ptr(I->m_view.rotMatrix()), modelView);
  MatrixTranslateC44f(modelView, -ori.x, -ori.y, -ori.z);
}

/*========================================================================*/
void SceneGetEyeNormal(PyMOLGlobals * G, float *v1, float *normal)
{
  CScene *I = G->Scene;
  float p1[4], p2[4];
  float modelView[16];

  SceneComposeModelViewMatrix(I, modelView);

  copy3f(v1, p1);
  p1[3] = 1.0;
  MatrixTransformC44f4f(modelView, p1, p2);     /* modelview transformation */
  copy3f(p2, p1);
  normalize3f(p1);
  MatrixInvTransformC44fAs33f3f(glm::value_ptr(I->m_view.rotMatrix()), p1, p2);
  invert3f3f(p2, normal);
}

/**
 * Return true if the v1 is within the safe clipping planes
 */
bool SceneGetVisible(PyMOLGlobals * G, const float *v1)
{
  CScene *I = G->Scene;
  float depth = SceneGetRawDepth(G, v1);
  return (I->m_view.m_clipSafe().m_back >= depth && depth >= I->m_view.m_clipSafe().m_front);
}

/**
 * Get the depth (camera space Z) of v1
 *
 * @param v1 point (3f) in world space or nullptr (= origin)
 */
float SceneGetRawDepth(PyMOLGlobals * G, const float *v1)
{
  CScene *I = G->Scene;
  float vt[3];
  float modelView[16];

  if(!v1 || SettingGetGlobal_i(G, cSetting_ortho))
    return -I->m_view.pos().z;

  SceneComposeModelViewMatrix(I, modelView);

  MatrixTransformC44f3f(modelView, v1, vt);
  return -vt[2];
}

/**
 * Get the depth (camera space Z) of v1 in normalized clip space
 * from 0.0 (near) to 1.0 (far)
 *
 * @param v1 point (3f) in world space or nullptr (= origin)
 */
float SceneGetDepth(PyMOLGlobals * G, const float *v1)
{
  CScene *I = G->Scene;
  float rawDepth = SceneGetRawDepth(G, v1);
  return ((rawDepth - I->m_view.m_clipSafe().m_front)/(I->m_view.m_clipSafe().m_back-I->m_view.m_clipSafe().m_front));
}

/*========================================================================*/
/**
 * Get the angstrom per pixel factor at v1. If v1 is nullptr, return the
 * factor at the origin, but clamped to an empirical positive value.
 *
 * @param v1 point (3f) in world space or nullptr (= origin)
 */
float SceneGetScreenVertexScale(PyMOLGlobals * G, const float *v1)

/* does not require OpenGL-provided matrices */
{
  float depth = SceneGetRawDepth(G, v1);
  float ratio = depth * GetFovWidth(G) / G->Scene->Height;

  if(!v1 && ratio < R_SMALL4)
    // origin depth, return a safe clipped value (origin must not be
    // behind or very close in front of the camera)
    ratio = R_SMALL4;

   return ratio;
}

void SceneRovingChanged(PyMOLGlobals * G)
{
  CScene *I = G->Scene;
  SceneRovingDirty(G);
  I->RovingCleanupFlag = true;
}

static void SceneRovingCleanup(PyMOLGlobals * G)
{
  CScene *I = G->Scene;
  const char *s;
  char buffer[OrthoLineLength];

  I->RovingCleanupFlag = false;

  s = SettingGet_s(G, nullptr, nullptr, cSetting_roving_selection);

  sprintf(buffer, "cmd.hide('lines','''%s''')", s);
  PParse(G, buffer);
  PFlush(G);
  sprintf(buffer, "cmd.hide('sticks','''%s''')", s);
  PParse(G, buffer);
  PFlush(G);
  sprintf(buffer, "cmd.hide('spheres','''%s''')", s);
  PParse(G, buffer);
  PFlush(G);
  sprintf(buffer, "cmd.hide('ribbon','''%s''')", s);
  PParse(G, buffer);
  PFlush(G);
  sprintf(buffer, "cmd.hide('cartoon','''%s''')", s);
  PParse(G, buffer);
  PFlush(G);
  sprintf(buffer, "cmd.hide('labels','''%s''')", s);
  PParse(G, buffer);
  PFlush(G);
  sprintf(buffer, "cmd.hide('nonbonded','''%s''')", s);
  PParse(G, buffer);
  PFlush(G);
  sprintf(buffer, "cmd.hide('nb_spheres','''%s''')", s);
  PParse(G, buffer);
  PFlush(G);
}

void SceneRovingUpdate(PyMOLGlobals * G)
{
  CScene *I = G->Scene;
  char buffer[OrthoLineLength];
  float sticks, lines, spheres, labels, ribbon, cartoon;
  float polar_contacts, polar_cutoff, nonbonded, nb_spheres;
  char byres[10] = "byres";
  char not_[4] = "not";
  char empty[1] = "";
  char *p1;
  char *p2;
  const char *s;
  int refresh_flag = false;
  const char *name;
  float level;
  float isosurface, isomesh;

  if(I->RovingDirtyFlag && ((UtilGetSeconds(G) - I->RovingLastUpdate) >
                            fabs(SettingGetGlobal_f(G, cSetting_roving_delay)))) {

    if(I->RovingCleanupFlag)
      SceneRovingCleanup(G);

    s = SettingGet_s(G, nullptr, nullptr, cSetting_roving_selection);
    sticks = SettingGetGlobal_f(G, cSetting_roving_sticks);
    lines = SettingGetGlobal_f(G, cSetting_roving_lines);
    labels = SettingGetGlobal_f(G, cSetting_roving_labels);
    spheres = SettingGetGlobal_f(G, cSetting_roving_spheres);
    ribbon = SettingGetGlobal_f(G, cSetting_roving_ribbon);
    cartoon = SettingGetGlobal_f(G, cSetting_roving_cartoon);
    polar_contacts = SettingGetGlobal_f(G, cSetting_roving_polar_contacts);
    polar_cutoff = SettingGetGlobal_f(G, cSetting_roving_polar_cutoff);
    nonbonded = SettingGetGlobal_f(G, cSetting_roving_nonbonded);
    nb_spheres = SettingGetGlobal_f(G, cSetting_roving_nb_spheres);

    isomesh = SettingGetGlobal_f(G, cSetting_roving_isomesh);
    isosurface = SettingGetGlobal_f(G, cSetting_roving_isosurface);

    if(SettingGetGlobal_b(G, cSetting_roving_byres))
      p2 = byres;
    else
      p2 = empty;

    if(sticks != 0.0F) {
      if(sticks < 0.0F) {
        p1 = not_;
        sticks = (float) fabs(sticks);
      } else {
        p1 = empty;
      }
      sprintf(buffer,
              "cmd.hide('sticks','''%s''');cmd.show('sticks','%s & enabled & %s %s (center expand %1.3f)')",
              s, s, p1, p2, sticks);
      PParse(G, buffer);
      PFlush(G);
      refresh_flag = true;
    }

    if(lines != 0.0F) {
      if(lines < 0.0F) {
        p1 = not_;
        lines = (float) fabs(lines);
      } else {
        p1 = empty;
      }
      sprintf(buffer,
              "cmd.hide('lines','''%s''');cmd.show('lines','%s & enabled & %s %s (center expand %1.3f)')",
              s, s, p1, p2, lines);
      PParse(G, buffer);
      PFlush(G);
      refresh_flag = true;
    }

    if(labels != 0.0F) {
      if(labels < 0.0F) {
        p1 = not_;
        labels = (float) fabs(labels);
      } else {
        p1 = empty;
      }
      sprintf(buffer,
              "cmd.hide('labels','''%s''');cmd.show('labels','%s & enabled & %s %s (center expand %1.3f)')",
              s, s, p1, p2, labels);
      PParse(G, buffer);
      PFlush(G);
      refresh_flag = true;
    }

    if(spheres != 0.0F) {
      if(spheres < 0.0F) {
        p1 = not_;
        spheres = (float) fabs(spheres);
      } else {
        p1 = empty;
      }
      sprintf(buffer,
              "cmd.hide('spheres','''%s''');cmd.show('spheres','%s & enabled & %s %s (center expand %1.3f)')",
              s, s, p1, p2, spheres);
      PParse(G, buffer);
      PFlush(G);
      refresh_flag = true;
    }

    if(cartoon != 0.0F) {
      if(cartoon < 0.0F) {
        p1 = not_;
        cartoon = (float) fabs(cartoon);
      } else {
        p1 = empty;
      }
      sprintf(buffer,
              "cmd.hide('cartoon','''%s''');cmd.show('cartoon','%s & enabled & %s %s (center expand %1.3f)')",
              s, s, p1, p2, cartoon);
      PParse(G, buffer);
      PFlush(G);
      refresh_flag = true;
    }

    if(ribbon != 0.0F) {
      if(ribbon < 0.0F) {
        p1 = not_;
        ribbon = (float) fabs(ribbon);
      } else {
        p1 = empty;
      }
      sprintf(buffer,
              "cmd.hide('ribbon','''%s''');cmd.show('ribbon','%s & enabled & %s %s (center expand %1.3f)')",
              s, s, p1, p2, ribbon);
      PParse(G, buffer);
      PFlush(G);

      refresh_flag = true;
    }

    if(polar_contacts != 0.0F) {
      int label_flag = 0;
      if(polar_contacts < 0.0F) {
        p1 = not_;
        polar_contacts = (float) fabs(polar_contacts);
      } else {
        p1 = empty;
      }
      if(polar_cutoff < 0.0F) {
        label_flag = true;
        polar_cutoff = (float) fabs(polar_cutoff);
      }
      sprintf(buffer,
              "cmd.delete('rov_pc');cmd.dist('rov_pc','%s & enabled & %s %s (center expand %1.3f)','same',%1.4f,mode=2,label=%d,quiet=2)",
              s, p1, p2, polar_contacts, polar_cutoff, label_flag);
      PParse(G, buffer);
      PFlush(G);

      refresh_flag = true;
    }

    if(nonbonded != 0.0F) {
      if(nonbonded < 0.0F) {
        p1 = not_;
        nonbonded = (float) fabs(nonbonded);
      } else {
        p1 = empty;
      }
      sprintf(buffer,
              "cmd.hide('nonbonded','''%s''');cmd.show('nonbonded','%s & enabled & %s %s (center expand %1.3f)')",
              s, s, p1, p2, nonbonded);
      PParse(G, buffer);
      PFlush(G);
      refresh_flag = true;
    }

    if(nb_spheres != 0.0F) {
      if(nb_spheres < 0.0F) {
        p1 = not_;
        nb_spheres = (float) fabs(nb_spheres);
      } else {
        p1 = empty;
      }
      sprintf(buffer,
              "cmd.hide('nb_spheres','''%s''');cmd.show('nb_spheres','%s & enabled & %s %s (center expand %1.3f)')",
              s, s, p1, p2, nb_spheres);
      PParse(G, buffer);
      PFlush(G);
      refresh_flag = true;
    }

    if(isomesh != 0.0F) {
      int auto_save;

      auto_save = SettingGetGlobal_i(G, cSetting_auto_zoom);
      SettingSetGlobal_i(G, cSetting_auto_zoom, 0);

      name = SettingGet_s(G, nullptr, nullptr, cSetting_roving_map1_name);
      if(name)
        if(name[0])
          if(ExecutiveFindObjectByName(G, name)) {
            level = SettingGetGlobal_f(G, cSetting_roving_map1_level);
            sprintf(buffer,
                    "cmd.isomesh('rov_m1','%s',%8.6f,'center',%1.3f)",
                    name, level, isomesh);
            PParse(G, buffer);
            PFlush(G);
            refresh_flag = true;
          }

      name = SettingGet_s(G, nullptr, nullptr, cSetting_roving_map2_name);
      if(name)
        if(name[0])
          if(ExecutiveFindObjectByName(G, name)) {
            level = SettingGetGlobal_f(G, cSetting_roving_map2_level);
            sprintf(buffer,
                    "cmd.isomesh('rov_m2','%s',%8.6f,'center',%1.3f)",
                    name, level, isomesh);
            PParse(G, buffer);
            PFlush(G);
            refresh_flag = true;
          }

      name = SettingGet_s(G, nullptr, nullptr, cSetting_roving_map3_name);
      if(name)
        if(name[0])
          if(ExecutiveFindObjectByName(G, name)) {
            level = SettingGetGlobal_f(G, cSetting_roving_map3_level);
            sprintf(buffer,
                    "cmd.isomesh('rov_m3','%s',%8.6f,'center',%1.3f)",
                    name, level, isomesh);
            PParse(G, buffer);
            PFlush(G);
            refresh_flag = true;
          }
      SettingSetGlobal_i(G, cSetting_auto_zoom, auto_save);
    }

    if(isosurface != 0.0F) {
      int auto_save;

      auto_save = SettingGetGlobal_i(G, cSetting_auto_zoom);
      SettingSetGlobal_i(G, cSetting_auto_zoom, 0);

      name = SettingGet_s(G, nullptr, nullptr, cSetting_roving_map1_name);
      if(name)
        if(name[0])
          if(ExecutiveFindObjectByName(G, name)) {
            level = SettingGetGlobal_f(G, cSetting_roving_map1_level);
            sprintf(buffer,
                    "cmd.isosurface('rov_s1','%s',%8.6f,'center',%1.3f)",
                    name, level, isosurface);
            PParse(G, buffer);
            PFlush(G);
            refresh_flag = true;
          }

      name = SettingGet_s(G, nullptr, nullptr, cSetting_roving_map2_name);
      if(name)
        if(name[0])
          if(ExecutiveFindObjectByName(G, name)) {
            level = SettingGetGlobal_f(G, cSetting_roving_map2_level);
            sprintf(buffer,
                    "cmd.isosurface('rov_s2','%s',%8.6f,'center',%1.3f)",
                    name, level, isosurface);
            PParse(G, buffer);
            PFlush(G);
            refresh_flag = true;
          }

      name = SettingGet_s(G, nullptr, nullptr, cSetting_roving_map3_name);
      if(name)
        if(name[0])
          if(ExecutiveFindObjectByName(G, name)) {
            level = SettingGetGlobal_f(G, cSetting_roving_map3_level);
            sprintf(buffer,
                    "cmd.isosurface('rov_s3','%s',%8.6f,'center',%1.3f)",
                    name, level, isosurface);
            PParse(G, buffer);
            PFlush(G);
            refresh_flag = true;
          }
      SettingSetGlobal_i(G, cSetting_auto_zoom, auto_save);
    }

    if(refresh_flag) {
      PParse(G, "cmd.refresh()");
      PFlush(G);
    }

    I->RovingLastUpdate = UtilGetSeconds(G);
    I->RovingDirtyFlag = false;
  }
}

/**
 * Will call cmd.raw_image_callback(img) with the current RGBA image, copied
 * to a WxHx4 numpy array. Return false if no callback is defined
 * (cmd.raw_image_callback == None).
 */
static
bool call_raw_image_callback(PyMOLGlobals * G) {
  bool done = false;

#ifndef _PYMOL_NOPY
  int blocked = PAutoBlock(G);
  auto raw_image_callback =
    PyObject_GetAttrString(G->P_inst->cmd, "raw_image_callback");

  if (raw_image_callback != Py_None) {
#ifdef _PYMOL_NUMPY
    auto& image = G->Scene->Image;

    // RGBA image as uint8 numpy array
    import_array1(0);
    npy_intp dims[3] = {image->getWidth(), image->getHeight(), 4};
    auto py = PyArray_SimpleNew(3, dims, NPY_UINT8);
    memcpy(PyArray_DATA((PyArrayObject *)py), image->bits(), dims[0] * dims[1] * 4);

    PYOBJECT_CALLFUNCTION(raw_image_callback, "O", py);
    Py_DECREF(py);

    done = true;
#else
    PRINTFB(G, FB_Scene, FB_Errors)
      " raw_image_callback-Error: no numpy support\n" ENDFB(G);
#endif
  }

  Py_XDECREF(raw_image_callback);
  PAutoUnblock(G, blocked);
#endif

  return done;
}

/**
 * Creates an image of the current scene.
 *
 * @param extent requested extent
 * @param antialias antialias mode
 * @param dpi dots per inch
 * @param format ??
 * @param quiet if true, don't print messages
 * @param out_img if not nullptr, store the image data here
 * @param filename if not empty, store the image to this file as PNG
 */

static void SceneImage(PyMOLGlobals* G, const Extent2D& extent, int antialias,
    float dpi, int format, bool quiet, pymol::Image* out_img,
    const std::string& filename)
{
  auto allButGizmos_i = pymol::to_underlying(SceneRenderWhich::All) &
                        ~pymol::to_underlying(SceneRenderWhich::Gizmos);
  auto allButGizmos = static_cast<SceneRenderWhich>(allButGizmos_i);
  SceneMakeSizedImage(G, extent, antialias, /*excludeSelecions*/ true, allButGizmos);
  if (!filename.empty()) {
    ScenePNG(G, filename.c_str(), dpi, quiet, false, format, nullptr);
  } else if (out_img) {
    png_outbuf_t outbuf;
    ScenePNG(G, "", dpi, quiet, false, format, &outbuf);
    out_img->setVecData(std::move(outbuf));
  } else if(call_raw_image_callback(G)) {
  } else if(G->HaveGUI && SettingGetGlobal_b(G, cSetting_auto_copy_images)) {
#ifdef _PYMOL_IP_EXTRAS
    if(IncentiveCopyToClipboard(G, di->quiet)) {
    }
#else
#ifdef PYMOL_EVAL
    PRINTFB(G, FB_Scene, FB_Warnings)
      " Warning: Clipboard image transfers disabled in Evaluation builds.\n" ENDFB(G);
#endif
#endif
  }
}

bool SceneDeferImage(PyMOLGlobals* G, const Extent2D& extent,
    const char* filename, int antialias, float dpi, int format, int quiet,
    pymol::Image* out_img)
{
  std::string filenameStr = filename ? filename : "";
  std::function<void()> deferred = [=]() {
    SceneImage(G, extent, antialias, dpi, format, quiet, out_img, filenameStr);
  };

  if (G->ValidContext) {
    deferred();
    return false;
  }

  OrthoDefer(G, std::move(deferred));
  return true;
}

int CScene::click(int button, int x, int y, int mod) // Originally SceneDeferClick!!
{
  return SceneDeferClickWhen(this, button, x, y, UtilGetSeconds(m_G), mod);
}

static int SceneDeferClickWhen(Block * block, int button, int x, int y, double when,
                               int mod)
{
  PyMOLGlobals *G = block->m_G;
  std::function<void()> deferred = [=]() {
    SceneClick(block, button, x, y, mod, when);
  };
  OrthoDefer(G, std::move(deferred));
  return 1;
}

int CScene::drag(int x, int y, int mod) //Originally SceneDeferDrag
{
  PyMOLGlobals *G = m_G;
  auto when = UtilGetSeconds(G);
  std::function<void()> deferred = [=]() {
    SceneDrag(this, x, y, mod, when);
  };
  OrthoDefer(G, std::move(deferred));
  return 1;
}

//static int SceneDeferredRelease(DeferredMouse * dm)
//{
//  SceneRelease(dm->block, dm->button, dm->x, dm->y, dm->mod, dm->when);
//  return 1;
//}

int CScene::release(int button, int x, int y, int mod) // Originally SceneDeferRelease
{
  PyMOLGlobals *G = m_G;
  auto when = UtilGetSeconds(G);
  std::function<void()> deferred = [=]() {
    SceneRelease(this, button, x, y, mod, when);
  };

  OrthoDefer(G, std::move(deferred));
  return 1;
}

/*========================================================================*/
void SceneFree(PyMOLGlobals * G)
{
  CScene *I = G->Scene;
#if !defined(PURE_OPENGL_ES_2) || defined(_WEBGL)
  CGOFree(I->offscreenCGO);
#endif

  CGOFree(I->AlphaCGO);
  CGOFree(I->offscreenCGO);
  CGOFree(I->offscreenOIT_CGO);
  CGOFree(I->offscreenOIT_CGO_copy);
  I->m_slots.clear();
  I->Obj.clear();
  I->GadgetObjs.clear();
  I->NonGadgetObjs.clear();

  ScenePurgeImage(G);
  CGOFree(G->DebugCGO);
  delete G->Scene;
}


/*========================================================================*/
void SceneResetMatrix(PyMOLGlobals * G)
{
  CScene *I = G->Scene;
  I->m_view.setRotMatrix(glm::mat4(1.0f));
  SceneUpdateInvMatrix(G);
}


/*========================================================================*/
void SceneSetDefaultView(PyMOLGlobals * G)
{
  CScene *I = G->Scene;

  I->m_view.setRotMatrix(glm::mat4(1.0f));
  SceneUpdateInvMatrix(G);

  I->ViewNormal[0] = 0.0F;
  I->ViewNormal[1] = 0.0F;
  I->ViewNormal[2] = 1.0F;

  I->m_view.setPos(0.0f, 0.0f, -50.0f);
  I->m_view.setOrigin(0.0f, 0.0f, 0.0f);

  I->m_view.m_clip().m_front = 40.0F;
  I->m_view.m_clip().m_back = 100.0F;
  UpdateFrontBackSafe(I);

  I->Scale = 1.0F;

}

int SceneReinitialize(PyMOLGlobals * G)
{
  int ok = true;
  SceneSetDefaultView(G);
  SceneCountFrames(G);
  SceneSetFrame(G, 0, 0);
  SceneInvalidate(G);
  G->Scene->SceneVec.clear();
  return (ok);
}


/*========================================================================*/
int SceneInit(PyMOLGlobals * G)
{
  CScene *I = nullptr;
  I = (G->Scene = new CScene(G));
  if(I) {
    assert(!I->RovingDirtyFlag);
    assert(I->DirtyFlag);

    /* all defaults to zero, so only initialize non-zero elements */
    G->DebugCGO = CGONew(G);

    I->LastClickTime = UtilGetSeconds(G);

    SceneSetDefaultView(G);

    I->active = true;

    OrthoAttach(G, I, cOrthoScene);

    I->LastRender = UtilGetSeconds(G);
    I->LastFrameTime = UtilGetSeconds(G);

    I->LastSweepTime = UtilGetSeconds(G);

    SceneRestartFrameTimer(G);
    SceneRestartPerfTimer(G);

    /* scene list */

    I->Pressed = -1;
    I->Over = -1;

    return 1;
  } else
    return 0;
}


/*========================================================================*/
void CScene::reshape(int width, int height)
{
  PyMOLGlobals *G = m_G;
  CScene *I = G->Scene;

  if(I->margin.right) {
    width -= I->margin.right;
    if(width < 1)
      width = 1;
  }

  if(I->margin.top) {
    height -= I->margin.top;
  }

  I->Width = width;

  I->Height = height;

  I->rect.top = I->Height;
  I->rect.left = 0;
  I->rect.bottom = 0;
  I->rect.right = I->Width;

  if(I->margin.bottom) {
    height -= I->margin.bottom;
    if(height < 1)
      height = 1;
    I->Height = height;
    I->rect.bottom = I->rect.top - I->Height;
  }
  SceneDirty(G);

  if(I->CopyType && (!I->CopyForced)) {
    SceneInvalidateCopy(G, false);
  }
  /*MovieClearImages(G); */
  MovieSetSize(G, I->Width, I->Height);
  SceneInvalidateStencil(G);
}


/*========================================================================*/
void SceneResetNormal(PyMOLGlobals * G, int lines)
{
  CScene *I = G->Scene;
  if(G->HaveGUI && G->ValidContext) {
    if(lines)
      glNormal3fv(I->LinesNormal);
    else
      glNormal3fv(I->ViewNormal);
  }
}

void SceneResetNormalCGO(PyMOLGlobals * G, CGO *cgo, int lines)
{
  CScene *I = G->Scene;
  if(G->HaveGUI && G->ValidContext) {
    if(lines)
      CGONormalv(cgo, I->LinesNormal);
    else
      CGONormalv(cgo, I->ViewNormal);
  }
}

void SceneResetNormalToViewVector(PyMOLGlobals * G, short use_shader)
{
  auto modMatrix = SceneGetModelViewMatrixPtr(G);
  if(G->HaveGUI && G->ValidContext) {
#if defined(PURE_OPENGL_ES_2)
    glVertexAttrib3f(VERTEX_NORMAL, modMatrix[2], modMatrix[6], modMatrix[10]);
#else
    if (use_shader){
      glVertexAttrib3f(VERTEX_NORMAL, modMatrix[2], modMatrix[6], modMatrix[10]);
    } else {
      glNormal3f(modMatrix[2], modMatrix[6], modMatrix[10]);
    }
#endif
  }
}

void SceneResetNormalUseShader(PyMOLGlobals * G, int lines, short use_shader)
{
  CScene *I = G->Scene;
  if(G->HaveGUI && G->ValidContext) {
#ifdef PURE_OPENGL_ES_2
    if(lines)
      glVertexAttrib3fv(VERTEX_NORMAL, I->LinesNormal);
    else
      glVertexAttrib3fv(VERTEX_NORMAL, I->ViewNormal);
#else
    if (use_shader){
      if(lines)
	glVertexAttrib3fv(VERTEX_NORMAL, I->LinesNormal);
      else
	glVertexAttrib3fv(VERTEX_NORMAL, I->ViewNormal);
    } else {
      if(lines)
	glNormal3fv(I->LinesNormal);
      else
	glNormal3fv(I->ViewNormal);
    }
#endif
  }
}

void SceneResetNormalUseShaderAttribute(PyMOLGlobals * G, int lines, short use_shader, int attr)
{
  CScene *I = G->Scene;
  if(G->HaveGUI && G->ValidContext) {
#ifdef PURE_OPENGL_ES_2
    if (attr < 0)
      return;
    if(lines)
      glVertexAttrib3fv(attr, I->LinesNormal);
    else
      glVertexAttrib3fv(attr, I->ViewNormal);
#else
    if (use_shader){
      if(lines)
	glVertexAttrib3fv(attr, I->LinesNormal);
      else
	glVertexAttrib3fv(attr, I->ViewNormal);
    } else {
      if(lines)
	glNormal3fv(I->LinesNormal);
      else
	glNormal3fv(I->ViewNormal);
    }
#endif
  }
}


void SceneGetResetNormal(PyMOLGlobals * G, float *normal, int lines)
{
  CScene *I = G->Scene;
  float *norm;
  if(G->HaveGUI && G->ValidContext) {
    if(lines)
      norm = I->LinesNormal;
    else
      norm = I->ViewNormal;
    normal[0] = norm[0]; normal[1] = norm[1]; normal[2] = norm[2];
  }
}

/*========================================================================*/
void SceneApplyImageGamma(PyMOLGlobals * G, unsigned int *buffer, int width,
                          int height)
{
  float gamma = SettingGetGlobal_f(G, cSetting_gamma);

  if(gamma > R_SMALL4)
    gamma = 1.0F / gamma;
  else
    gamma = 1.0F;

  if(buffer && height && width) {
    float _inv3 = 1 / (255 * 3.0F);
    float _1 = 1 / 3.0F;
    unsigned char *p;
    int x, y;
    float c1, c2, c3, inp, sig;
    unsigned int i1, i2, i3;
    p = (unsigned char *) buffer;
    for(y = 0; y < height; y++) {
      for(x = 0; x < width; x++) {
        c1 = p[0];
        c2 = p[1];
        c3 = p[2];
        inp = (c1 + c2 + c3) * _inv3;
        if(inp < R_SMALL4)
          sig = _1;
        else
          sig = (float) (pow(inp, gamma) / inp);
        i1 = (unsigned int) (sig * c1);
        i2 = (unsigned int) (sig * c2);
        i3 = (unsigned int) (sig * c3);
        if(i1 > 255)
          i1 = 255;
        if(i2 > 255)
          i2 = 255;
        if(i3 > 255)
          i3 = 255;
        p[0] = i1;
        p[1] = i2;
        p[2] = i3;
        p += 4;
      }
    }
  }
}

void SceneUpdateAnimation(PyMOLGlobals * G)
{
  CScene *I = G->Scene;
  int rockFlag = false;
  int dirty = false;
  int movie_rock = SettingGetGlobal_i(G, cSetting_movie_rock);

  if(movie_rock < 0)
    movie_rock = ControlRocking(G);

  if(MoviePlaying(G) && movie_rock) {

    if(MovieGetRealtime(G) && !SettingGetGlobal_b(G, cSetting_movie_animate_by_frame)) {
      I->RenderTime = UtilGetSeconds(G) - I->LastSweepTime;
      rockFlag = true;
      dirty = true;             /* force a subsequent update */
    } else {
      float fps = SceneGetFPS(G);       /* guaranteed to be >= 0.0F */
      if(fps > 0.0F) {
        int rock_frame = SceneGetFrame(G);
        if(rock_frame != I->RockFrame) {
          I->RockFrame = rock_frame;
          rockFlag = true;
          I->RenderTime = 1.0 / fps;
        }
      } else {
        I->RenderTime = UtilGetSeconds(G) - I->LastSweepTime;
        rockFlag = true;
      }
    }
  } else
    dirty = true;

  if(I->cur_ani_elem < I->n_ani_elem) { /* play motion animation */
    double now;

    int cur = I->cur_ani_elem;

    if(I->AnimationStartFlag) {
      /* allow animation timing to lag since it may take a few seconds
         to get here given geometry updates, etc. */

      I->AnimationLagTime = UtilGetSeconds(G) - I->AnimationStartTime;
      I->AnimationStartFlag = false;
    }

    if((!MoviePlaying(G)) ||
       ((MovieGetRealtime(G) &&
         !SettingGetGlobal_b(G, cSetting_movie_animate_by_frame)))) {
      now = UtilGetSeconds(G) - I->AnimationLagTime;
    } else {
      float fps = SceneGetFPS(G);       /* guaranteed to be >= 0.0F */
      int frame = SceneGetFrame(G);
      int n_frame = 0;

      cur = 0;                  /* allow backwards interpolation */
      if(frame >= I->AnimationStartFrame) {
        n_frame = frame - I->AnimationStartFrame;
      } else {
        n_frame = frame + (I->NFrame - I->AnimationStartFrame);
      }
      now = I->AnimationStartTime + n_frame / fps;
    }

    while(I->ani_elem[cur].timing < now) {
      cur++;
      if(cur >= I->n_ani_elem) {
        cur = I->n_ani_elem;
        break;
      }
    }
    I->cur_ani_elem = cur;
    SceneFromViewElem(G, I->ani_elem + cur, dirty);
    OrthoDirty(G);
  }
  if(rockFlag && (I->RenderTime != 0.0)) {
    SceneUpdateCameraRock(G, dirty);
  }
}

int SceneGetDrawFlag(GridInfo * grid, int *slot_vla, int slot)
{
  int draw_flag = false;
  if(grid && grid->active) {
    switch (grid->mode) {
    case GridMode::ByObject: /* assigned grid slots (usually by group) */
      {
        if(((slot < 0) && grid->slot) ||
           ((slot == 0) && (grid->slot == 0)) ||
           (slot_vla && (slot >= 0 && slot_vla[slot] == grid->slot))) {
          draw_flag = true;
        }
      }
      break;
    case GridMode::ByObjectStates:
    case GridMode::ByObjectByState:
      draw_flag = true;
      break;
    }
  } else {
    draw_flag = true;
  }
  return draw_flag;
}

int SceneGetDrawFlagGrid(PyMOLGlobals * G, GridInfo * grid, int slot)
{
  CScene *I = G->Scene;
  return SceneGetDrawFlag(grid, I->m_slots.data(), slot);
}

/*========================================================================*/
void SceneCopy(PyMOLGlobals * G, GLFramebufferConfig config, int force, int entire_window)
{
  CScene *I = G->Scene;

  if(force || (!(I->StereoMode ||
                 SettingGetGlobal_b(G, cSetting_stereo_double_pump_mono) || I->ButtonsShown))) {
    /* no copies while in stereo mode */
    if(force || ((!I->DirtyFlag) && (!I->CopyType))) {
      Rect2D rect;
      if(entire_window) {
        rect = OrthoGetRect(G);
      } else {
        rect = Rect2D{{}, SceneGetExtent(G)};
      }
      ScenePurgeImage(G);
      auto imgData = G->ShaderMgr->readPixelsFrom(G, rect, config);
      if (!imgData.empty()) {
        I->Image = std::make_shared<pymol::Image>(rect.extent.width, rect.extent.height);
        I->Image->setVecData(std::move(imgData));
      }
      I->CopyType = true;
      I->Image->m_needs_alpha_reset = true;
      I->CopyForced = force;
    }
  }
}

/*========================================================================*/
int SceneRovingCheckDirty(PyMOLGlobals * G)
{
  CScene *I = G->Scene;
  return (I->RovingDirtyFlag);
}

struct _CObjectUpdateThreadInfo {
  pymol::CObject *obj;
};

void SceneObjectUpdateThread(CObjectUpdateThreadInfo * T)
{
  if(T->obj) {
    T->obj->update();
  }
}

#ifndef _PYMOL_NOPY
static void SceneObjectUpdateSpawn(PyMOLGlobals * G, CObjectUpdateThreadInfo * Thread,
                                   int n_thread, int n_total)
{
  if(n_total == 1) {
    SceneObjectUpdateThread(Thread);
  } else if(n_total) {
    int blocked;
    PyObject *info_list;
    int a, n = 0;
    blocked = PAutoBlock(G);

    PRINTFB(G, FB_Scene, FB_Blather)
      " Scene: updating objects with %d threads...\n", n_thread ENDFB(G);
    info_list = PyList_New(n_total);
    for(a = 0; a < n_total; a++) {
      PyList_SetItem(info_list, a, PyCapsule_New(Thread + a, nullptr, nullptr));
      n++;
    }
    PXDecRef(PYOBJECT_CALLMETHOD
             (G->P_inst->cmd, "_object_update_spawn", "Oi", info_list, n_thread));
    Py_DECREF(info_list);
    PAutoUnblock(G, blocked);
  }
}
#endif

static void SceneStencilCheck(PyMOLGlobals *G) 
{
  CScene *I = G->Scene;
  if( I->StereoMode == cStereo_stencil_by_row ) {
    int bottom = 0;

#ifndef _PYMOL_PRETEND_GLUT
    if(G->Main)
      bottom = p_glutGet(P_GLUT_WINDOW_Y) + p_glutGet(P_GLUT_WINDOW_HEIGHT);
#endif

    int parity = bottom & 0x1;
    if(parity != I->StencilParity) {
      I->StencilValid = false;
      I->StencilParity = parity;
      SceneDirty(G);
    }
  }
}

/*========================================================================*/
void SceneUpdate(PyMOLGlobals * G, int force)
{
  CScene *I = G->Scene;

  int cur_state = SettingGetGlobal_i(G, cSetting_state) - 1;
  int defer_builds_mode = SettingGetGlobal_i(G, cSetting_defer_builds_mode);

  PRINTFD(G, FB_Scene)
    " SceneUpdate: entered.\n" ENDFD;

  OrthoBusyPrime(G);
  WizardDoPosition(G, false);
  WizardDoView(G, false);
  EditorUpdate(G);
  SceneStencilCheck(G);

  if(defer_builds_mode == 0) {
    if(SettingGetGlobal_i(G, cSetting_draw_mode) == -2) {
      defer_builds_mode = 1;
    }
  }

  if(force || I->ChangedFlag || ((cur_state != I->LastStateBuilt) &&
                                 (defer_builds_mode > 0))) {

    SceneCountFrames(G);

    if(force || (defer_builds_mode != 5)) {     /* mode 5 == immediate mode */

      PyMOL_SetBusy(G->PyMOL, true);    /*  race condition -- may need to be fixed */

      /* update all gadgets first (single-threaded since they're thread-unsafe) */
      for (auto& GadgetObj : I->GadgetObjs) {
        GadgetObj->update();
      }

      {
#ifndef _PYMOL_NOPY
        int n_thread = SettingGetGlobal_i(G, cSetting_max_threads);
        int multithread = SettingGetGlobal_i(G, cSetting_async_builds);
        if(multithread && (n_thread > 1)) {
          int min_start = -1;
          int max_stop = -1;
          int n_obj = 0;
          for (auto& obj : I->Obj) {
            int start = 0;
            n_obj++;
            int stop = obj->getNFrame();
	    /* set start/stop to define the range for this object
	     * depending upon various build settings */
            ObjectAdjustStateRebuildRange(obj, &start, &stop);
            if(min_start < 0) {
              min_start = start;
              max_stop = stop;
            } else {
              if(min_start > start)
                min_start = start;
              if(max_stop < stop)
                max_stop = stop;
            }
          }

          int n_frame = max_stop - min_start;

          if(n_frame > n_thread) {
            n_thread = 1;
            /* prevent n_thread * n_thread -- only multithread within
               individual object states (typically more balanced) */
          } else if(n_frame > 1) {
            n_thread = n_thread / n_frame;
          }

          if(n_thread < 1)
            n_thread = 1;
        }

	/* Note: we might want to optimize this by doing multi-threaded updates
	   for all objects. */
        if(multithread && (n_thread > 1)) {
          /* multi-threaded geometry update */
          int cnt = I->NonGadgetObjs.size();

          if(cnt) {
            CObjectUpdateThreadInfo *thread_info = pymol::malloc<CObjectUpdateThreadInfo>(cnt);
            if(thread_info) {
              cnt = 0;
              for (auto& NonGadgetObj : I->NonGadgetObjs) {
                thread_info[cnt++].obj = NonGadgetObj;
              }
              SceneObjectUpdateSpawn(G, thread_info, n_thread, cnt);
              FreeP(thread_info);
            }
          }
        } else
#endif
          /* single-threaded update */
          for (auto& obj : I->Obj) {
            obj->update();
          }
      }
      PyMOL_SetBusy(G->PyMOL, false);   /*  race condition -- may need to be fixed */
    } else { /* defer builds mode == 5 -- for now, only update non-molecular objects */
      /* single-threaded update */
      for (auto& obj : I->Obj) {
        if(obj->type != cObjectMolecule) {
          obj->update();
        }
      }
    }

    I->ChangedFlag = false;

    if((defer_builds_mode >= 2) && (force || (defer_builds_mode != 5)) &&
       (cur_state != I->LastStateBuilt)) {
      /* purge graphics representation when no longer used */
      if(I->LastStateBuilt >= 0) {
        for ( auto it = I->Obj.begin(); it != I->Obj.end(); ++it) {
          if ((*it)->type != cObjectMolecule || force || defer_builds_mode != 5) {
            int static_singletons =
              SettingGet_b(G, (*it)->Setting.get(), nullptr, cSetting_static_singletons);
            int async_builds =
              SettingGet_b(G, (*it)->Setting.get(), nullptr, cSetting_async_builds);
            int max_threads =
              SettingGet_i(G, (*it)->Setting.get(), nullptr, cSetting_max_threads);
            int nFrame = 0;
            nFrame = (*it)->getNFrame();
            if((nFrame > 1) || (!static_singletons)) {
              int start = I->LastStateBuilt;
              int stop = start + 1;
              int ste;
              if(async_builds && (max_threads > 1)) {
                if((start / max_threads) == (cur_state / max_threads)) {
                  stop = start; /* don't purge current batch */
                } else {
                  int base = start / max_threads;       /* now purge previous batch */
                  start = base * max_threads;
                  stop = (base + 1) * max_threads;
                }
              }
              for(ste = start; ste < stop; ste++) {
                (*it)->invalidate(cRepAll, cRepInvPurge, ste);
              }
            }
          }
        }
      }
    }
    I->LastStateBuilt = cur_state;
    WizardDoScene(G);
    if(!MovieDefined(G)) {
      if(SettingGetGlobal_i(G, cSetting_frame) != (cur_state + 1))
        SettingSetGlobal_i(G, cSetting_frame, (cur_state + 1));
    }
  }

  PRINTFD(G, FB_Scene)
    " %s: leaving...\n", __func__ ENDFD;
}


/*========================================================================*/
int SceneRenderCached(PyMOLGlobals * G)
{
  /* sets up a cached image buffer is one is available, or if we are
   * using cached images by default */
  CScene *I = G->Scene;
  std::shared_ptr<pymol::Image> image;
  int renderedFlag = false;
  int draw_mode = SettingGetGlobal_i(G, cSetting_draw_mode);
  PRINTFD(G, FB_Scene)
    " %s: entered.\n", __func__ ENDFD;

  G->ShaderMgr->Check_Reload();
  if(I->DirtyFlag) {
    int moviePlaying = MoviePlaying(G);

    if(I->MovieFrameFlag || (moviePlaying && SettingGetGlobal_b(G, cSetting_cache_frames))) {
      I->MovieFrameFlag = false;
      image = MovieGetImage(G,
                            MovieFrameToImage(G,
                                              SettingGetGlobal_i(G, cSetting_frame) - 1));
      if(image) {
        if(I->Image){
          ScenePurgeImage(G);
        }
        I->CopyType = true;
        I->Image = image;
        OrthoDirty(G);
        renderedFlag = true;
      } else {
        SceneMakeMovieImage(G, true, false, cSceneImage_Default);
        renderedFlag = true;
      }
    } else if(draw_mode == 3) {
      auto show_progress = SettingGet<int>(G, cSetting_show_progress);
      SettingSetGlobal_i(G, cSetting_show_progress, 0);
      SceneRay(G, 0, 0, SettingGetGlobal_i(G, cSetting_ray_default_renderer),
               nullptr, nullptr, 0.0F, 0.0F, false, nullptr, false, -1);
      SettingSetGlobal_i(G,cSetting_show_progress, show_progress);
    } else if(moviePlaying && SettingGetGlobal_b(G, cSetting_ray_trace_frames)) {
      SceneRay(G, 0, 0, SettingGetGlobal_i(G, cSetting_ray_default_renderer),
               nullptr, nullptr, 0.0F, 0.0F, false, nullptr, true, -1);
    } else if((moviePlaying && SettingGetGlobal_b(G, cSetting_draw_frames)) || (draw_mode == 2)) {
      Extent2D extent {0u, 0u};
      SceneMakeSizedImage(G, extent, SettingGetGlobal_i(G, cSetting_antialias), /*excludeSelections*/ false);
    } else if(I->CopyType == true) {    /* true vs. 2 */
      renderedFlag = true;
    } else {
      renderedFlag = false;
    }
  } else if(I->CopyType == true) {      /* true vs. 2 */
    renderedFlag = true;
  }

  PRINTFD(G, FB_Scene)
    " %s: leaving...renderedFlag %d\n", __func__, renderedFlag ENDFD;

  return (renderedFlag);
}

float SceneGetSpecularValue(PyMOLGlobals * G, float spec, int limit)
{
  int n_light = SettingGetGlobal_i(G, cSetting_spec_count);
  if(n_light < 0)
    n_light = SettingGetGlobal_i(G, cSetting_light_count);
  if(n_light > limit)
    n_light = limit;
  if(n_light > 2) {
    spec = spec / pow(n_light - 1, 0.6F);
  }
  return (spec > 1.F) ? 1.F : (spec < 0.F) ? 0.F : spec;
}

float SceneGetReflectScaleValue(PyMOLGlobals * G, int limit)
{
  float result = 1.0F;
  int n_light = SettingGetGlobal_i(G, cSetting_light_count);
  if(n_light > limit)
    n_light = limit;
  if(n_light > 1) {
    float tmp[3];
    float sum = 0.0F;
    for (int i = 0; i < n_light - 1; ++i) {
      copy3f(SettingGetGlobal_3fv(G, light_setting_indices[i]), tmp);
      normalize3f(tmp);
      sum += 1.f - tmp[2];
    }
    sum *= 0.5;
    return result / sum;
  }
  return result;
}

void SceneGetModel2WorldMatrix(PyMOLGlobals * G, float *matrix) {
  CScene *I = G->Scene;
  if (!I)
    return;

  identity44f(matrix);
  const auto& pos = I->m_view.pos();
  const auto& ori = I->m_view.origin();
  MatrixTranslateC44f(matrix, pos.x, pos.y, pos.z);
  MatrixMultiplyC44f(glm::value_ptr(I->m_view.rotMatrix()), matrix);
  MatrixTranslateC44f(matrix, -ori.x, -ori.y, -ori.z);
}

void SceneSetModel2WorldMatrix(PyMOLGlobals * G, float const *matrix) {
  CScene *I = G->Scene;
  if (!I)
    return;

  // build inverse origin translate
  float invOriginTranslate[16];  
  identity44f(invOriginTranslate);
  const auto& ori = I->m_view.origin();
  MatrixTranslateC44f(invOriginTranslate, ori.x, ori.y, ori.z);
  // get shiftRot from m2wNew
  float temp[16];
  memcpy(temp, matrix, sizeof(temp));  
  MatrixMultiplyC44f(invOriginTranslate, temp);
  I->m_view.setPos(temp[12], temp[13], temp[14]);
  // decompose shiftRot
  temp[12] = temp[13] = temp[14] = 0.0f;
  I->m_view.setRotMatrix(glm::make_mat4(temp));
}

/**
 * Get specular and shininess, adjusted to the number of lights.
 *
 * @param[out] ptr_spec              specular for lights 2-N
 * @param[out] ptr_spec_power        shininess for lights 2-N
 * @param[out] ptr_spec_direct       specular for light 1
 * @param[out] ptr_spec_direct_power shininess for light 1
 * @param limit number of lights (e.g. `light_count` setting)
 */
void SceneGetAdjustedLightValues(PyMOLGlobals * G,
    float *ptr_spec,
    float *ptr_spec_power,
    float *ptr_spec_direct,
    float *ptr_spec_direct_power,
    int limit)
{
  float specular = SettingGetGlobal_f(G, cSetting_specular);
  if (specular == 1.0F)
    specular = SettingGetGlobal_f(G, cSetting_specular_intensity);
  if (specular < R_SMALL4)
    specular = 0.0F;

  float spec_power = SettingGetGlobal_f(G, cSetting_spec_power);
  if (spec_power < 0.0F)
    spec_power = SettingGetGlobal_f(G, cSetting_shininess);

  float spec_reflect = SettingGetGlobal_f(G, cSetting_spec_reflect);
  if (spec_reflect < 0.0F)
    spec_reflect = specular;

  float spec_direct = SettingGetGlobal_f(G, cSetting_spec_direct);
  if (spec_direct < 0.0F)
    spec_direct = specular;

  float spec_direct_power = SettingGetGlobal_f(G, cSetting_spec_direct_power);
  if (spec_direct_power < 0.0F)
    spec_direct_power = spec_power;

  *ptr_spec = SceneGetSpecularValue(G, spec_reflect, limit);
  *ptr_spec_power = spec_power;
  *ptr_spec_direct = spec_direct > 1.F ? 1.F : spec_direct;
  *ptr_spec_direct_power = spec_direct_power;
}

/*
 * Shader attribute names
 */
#define TEMPLATE(i) "g_LightSource[" #i "].position"
const char * lightsource_position_names[] = {
  TEMPLATE(0), TEMPLATE(1), TEMPLATE(2), TEMPLATE(3), TEMPLATE(4),
  TEMPLATE(5), TEMPLATE(6), TEMPLATE(7), TEMPLATE(8), TEMPLATE(9)
};
#undef TEMPLATE

#define TEMPLATE(i) "g_LightSource[" #i "].diffuse"
const char * lightsource_diffuse_names[] = {
  TEMPLATE(0), TEMPLATE(1), TEMPLATE(2), TEMPLATE(3), TEMPLATE(4),
  TEMPLATE(5), TEMPLATE(6), TEMPLATE(7), TEMPLATE(8), TEMPLATE(9)
};
#undef TEMPLATE

/**
 * Sets up lighting for immediate mode if shaderPrg=nullptr, otherwise
 * sets lighting uniforms for the given shader program.
 *
 * Supports up to light_count=8
 */
void SceneProgramLighting(PyMOLGlobals * G, CShaderPrg * shaderPrg)
{

  /* load up the light positions relative to the camera while 
     MODELVIEW still has the identity */
  int n_light = glm::clamp(SettingGetGlobal_i(G, cSetting_light_count), 0, 8);
  int spec_count = SettingGetGlobal_i(G, cSetting_spec_count);
  float direct = SettingGetGlobal_f(G, cSetting_direct);
  float reflect = SettingGetGlobal_f(G, cSetting_reflect) * SceneGetReflectScaleValue(G, n_light);
  float spec[4];
  float diff[4];
  const float zero[4] = { 0.0F, 0.0F, 0.0F, 1.0F };
  float vv[4] = {0.F, 0.F, 1.F, 0.F}; // position
  float spec_value, shine, spec_direct, spec_direct_power;

  SceneGetAdjustedLightValues(G,
      &spec_value,
      &shine,
      &spec_direct,
      &spec_direct_power,
      n_light);

  if (n_light < 2) {
    direct += reflect;
    if(direct > 1.0F)
      direct = 1.0F;
  }

  if(spec_count < 0) {
    spec_count = n_light;
  }

  // light 0

  white4f(diff, SettingGetGlobal_f(G, cSetting_ambient));

#ifndef PURE_OPENGL_ES_2
  if (!shaderPrg) {
    glEnable(GL_LIGHTING);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, diff);
    glLightfv(GL_LIGHT0, GL_POSITION, vv);
    glLightfv(GL_LIGHT0, GL_AMBIENT, zero);
    if(direct > R_SMALL4) {
      white4f(diff, direct);
      white4f(spec, spec_direct);
      glEnable(GL_LIGHT0);
      glLightfv(GL_LIGHT0, GL_DIFFUSE, diff);
      glLightfv(GL_LIGHT0, GL_SPECULAR, spec);
    } else {
      glLightfv(GL_LIGHT0, GL_DIFFUSE, zero);
      glLightfv(GL_LIGHT0, GL_SPECULAR, zero);
    }
  } else
#endif
  {
    shaderPrg->Set4fv("g_LightModel.ambient", diff);
    white4f(diff, (direct > R_SMALL4) ? direct : 0.f);
    shaderPrg->Set4fv(lightsource_diffuse_names[0], diff);
    shaderPrg->Set4fv(lightsource_position_names[0], vv);
  }

  // light 1-N

  white4f(spec, spec_value);
  white4f(diff, reflect);

  for (int i = 1; i < n_light; ++i) {
    // normalized/inverted light direction
    copy3f(SettingGetGlobal_3fv(G, light_setting_indices[i - 1]), vv);
    normalize3f(vv);
    invert3f(vv);

#ifndef PURE_OPENGL_ES_2
    if (!shaderPrg) {
      glEnable(GL_LIGHT0 + i);
      glLightfv(GL_LIGHT0 + i, GL_POSITION, vv);
      glLightfv(GL_LIGHT0 + i, GL_SPECULAR, (spec_count >= i) ? spec : zero);
      glLightfv(GL_LIGHT0 + i, GL_AMBIENT, zero);
      glLightfv(GL_LIGHT0 + i, GL_DIFFUSE, diff);
    } else
#endif
    {
      shaderPrg->Set4fv(lightsource_position_names[i], vv);
      shaderPrg->Set4fv(lightsource_diffuse_names[i], diff);
    }
  }

#ifndef PURE_OPENGL_ES_2
  if (!shaderPrg) {
    // TODO: this was depending on two_sided_lighting, surface_cavity_mode
    // and transparency_mode
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_FALSE);

    // disable unused lights
    for (int i = 7; i >= n_light; --i) {
      glDisable(GL_LIGHT0 + i);
    }

    // material
    white4f(spec, 1.F);
    glMaterialfv(GL_FRONT, GL_SPECULAR, spec);
    glMaterialf(GL_FRONT, GL_SHININESS, glm::clamp(shine, 0.F, 128.F));
  }
#endif
}

/**
 * Set up the Scene Fog* member variables and immediate mode fog (incl.
 * gl_Fog struct for non-ES2 shaders)
 */
int SceneSetFog(PyMOLGlobals *G){
  CScene *I = G->Scene;
  int fog_active = false;
  float fog_density = SettingGetGlobal_f(G, cSetting_fog);
  I->FogStart = (I->m_view.m_clipSafe().m_back - I->m_view.m_clipSafe().m_front) * SettingGetGlobal_f(G, cSetting_fog_start) + I->m_view.m_clipSafe().m_front;
  if((fog_density > R_SMALL8) && (fog_density != 1.0F)) {
    I->FogEnd = I->FogStart + (I->m_view.m_clipSafe().m_back - I->FogStart) / fog_density;
  } else {
    I->FogEnd = I->m_view.m_clipSafe().m_back;
  }
  
  if(SettingGetGlobal_b(G, cSetting_depth_cue) && fog_density != 0.0F) {
    fog_active = true;
  }

#ifndef PURE_OPENGL_ES_2
  if (ALWAYS_IMMEDIATE_OR(!SettingGetGlobal_b(G, cSetting_use_shaders))) {
  const float *bg_rgb = ColorGet(G, SettingGetGlobal_color(G, cSetting_bg_rgb));
  float fog[4] = {bg_rgb[0], bg_rgb[1], bg_rgb[2], 1.0};

  glFogf(GL_FOG_MODE, GL_LINEAR);
  glFogf(GL_FOG_START, I->FogStart);
  glFogf(GL_FOG_END, I->FogEnd);
  glFogf(GL_FOG_DENSITY, fog_density > R_SMALL8 ? fog_density : 1.0F);
  glFogfv(GL_FOG_COLOR, fog);
  if (fog_active)
    glEnable(GL_FOG);
  else
    glDisable(GL_FOG);
  }
#endif

  return fog_active;
}

/**
 * Set the g_Fog_* uniforms for ES2 shaders
 */
void SceneSetFogUniforms(PyMOLGlobals * G, CShaderPrg * shaderPrg) {
  CScene *I = G->Scene;
  if (shaderPrg) {
    float fogScale = 1.0f / (I->FogEnd - I->FogStart);
    shaderPrg->Set1f("g_Fog_end", I->FogEnd);
    shaderPrg->Set1f("g_Fog_scale", fogScale);
  }
}

void SceneSetupGLPicking(PyMOLGlobals * G){
      /* picking mode: we want flat, unshaded, unblended, unsmooth colors */

      glDisable(GL_FOG);
      glDisable(GL_COLOR_MATERIAL);
      glDisable(GL_LIGHTING);
      glDisable(GL_LINE_SMOOTH);
      glDisable(GL_DITHER);
      glDisable(GL_BLEND);
      glDisable(GL_POLYGON_SMOOTH);
      if(G->Option->multisample)
        glDisable(0x809D);      /* GL_MULTISAMPLE_ARB */
      glShadeModel(GL_FLAT);
}

/*========================================================================*/
void SceneRestartFrameTimer(PyMOLGlobals * G)
{
  CScene *I = G->Scene;
  I->LastFrameTime = UtilGetSeconds(G);
}

static void SceneRestartPerfTimer(PyMOLGlobals * G)
{
  CScene *I = G->Scene;
  I->LastRender = UtilGetSeconds(G);
  I->RenderTime = 0.0;
}

void SceneRestartSweepTimer(PyMOLGlobals * G)
{
  CScene *I = G->Scene;
  I->LastSweep = 0.0F;          /* continue to defer rocking until this is done */
  I->LastSweepX = 0.0F;
  I->LastSweepY = 0.0F;
  I->SweepTime = 0.0;
  I->LastSweepTime = UtilGetSeconds(G);
  SceneRestartPerfTimer(G);

}


/*========================================================================*/
void ScenePrepareMatrix(PyMOLGlobals * G, int mode, int stereo_mode /* = 0 */)
{
  CScene *I = G->Scene;

  float stAng, stShift;
  const auto& pos = I->m_view.pos();
  const auto& ori = I->m_view.origin();

#ifdef _PYMOL_OPENVR
  bool isOpenVR = (stereo_mode == cStereo_openvr) && OpenVRReady(G);

  if(isOpenVR) {

    /* stereo OpenVR */

    if (!mode) {
      // average projection matrix for picking
      glMatrixMode(GL_PROJECTION);
      OpenVRLoadPickingProjectionMatrix(G, I->m_view.m_clipSafe().m_front, I->m_view.m_clipSafe().m_back);

      // mono matrix for picking
      glMatrixMode(GL_MODELVIEW);
      glLoadMatrixf(OpenVRGetPickingMatrix(G));

    } else {
      glMatrixMode(GL_PROJECTION);
      OpenVRLoadProjectionMatrix(G, I->m_view.m_clipSafe().m_front, I->m_view.m_clipSafe().m_back);

      glMatrixMode(GL_MODELVIEW);
      OpenVRLoadWorld2EyeMatrix(G);

    }

    if (OpenVRIsMoleculeCaptured(G)) {
      float scaler;
      float const *mol2world = OpenVRGetMolecule2WorldMatrix(G, &scaler);
      // save old plane shifts
      float dist = fabsf(pos.z);
      float frontShift = fabsf(dist - I->m_view.m_clip().m_front);
      float backShift = fabsf(dist - I->m_view.m_clip().m_back);
      // apply new transform to molecule
      SceneSetModel2WorldMatrix(G, mol2world);
      SceneScale(G, scaler);
      // renew front and back planes
      dist = fabsf(pos.z);
      SceneClipSet(G, dist - frontShift * scaler, dist + backShift * scaler);
    }

    /* move the camera to the location we are looking at */
    glTranslatef(pos.x, pos.y, pos.z);

    /* scale molecule */
    glScalef(I->Scale, I->Scale, I->Scale);

    /* rotate about the origin (the the center of rotation) */
    glMultMatrixf(glm::value_ptr(I->m_view.rotMatrix()));

    /* move the origin to the center of rotation */
    glTranslatef(-ori.x, -ori.y, -ori.z);

    // TODO don't do the immediate mode detour
    glGetFloatv(GL_PROJECTION_MATRIX, SceneGetProjectionMatrixPtr(G));
    glGetFloatv(GL_MODELVIEW_MATRIX, SceneGetModelViewMatrixPtr(G));

  } else
#endif
  {
    if (!mode){
      SceneComposeModelViewMatrix(I, SceneGetModelViewMatrixPtr(G));
    } else {
      /* stereo */
      float tmpMatrix[16];
      stAng = SettingGetGlobal_f(G, cSetting_stereo_angle);// * cPI / 180.f;
      stShift = SettingGetGlobal_f(G, cSetting_stereo_shift);

      stShift = (float) (stShift * fabs(pos.z) / 100.0);
      stAng = (float) (-stAng * atan(stShift / fabs(pos.z)) / 2.f);

      if(mode == 2) {             /* left hand */
	stAng = -stAng;
	stShift = -stShift;
      }
    
      PRINTFD(G, FB_Scene)
	" StereoMatrix-Debug: mode %d stAng %8.3f stShift %8.3f \n", mode, stAng, stShift
	ENDFD;
      identity44f(tmpMatrix);
      I->modelViewMatrix = glm::mat4(1.0f);
      MatrixRotateC44f(SceneGetModelViewMatrixPtr(G), stAng, 0.f, 1.f, 0.f);
      MatrixTranslateC44f(tmpMatrix, pos.x + stShift, pos.y, pos.z);
      MatrixMultiplyC44f(tmpMatrix, SceneGetModelViewMatrixPtr(G));
      MatrixMultiplyC44f(glm::value_ptr(I->m_view.rotMatrix()), SceneGetModelViewMatrixPtr(G));
      MatrixTranslateC44f(SceneGetModelViewMatrixPtr(G), -ori.x, -ori.y, -ori.z);

    }
  }

#ifndef PURE_OPENGL_ES_2
  if (ALWAYS_IMMEDIATE_OR(!SettingGetGlobal_b(G, cSetting_use_shaders))) {
    glLoadMatrixf(SceneGetModelViewMatrixPtr(G));
  }
#endif
}


/*========================================================================*/
/**
 * Update the scene rotation matrix (m_view.m_rotMatrix)
 *
 * @param angle Angle in degrees
 * @param x,y,z Axis
 * @param dirty Call SceneInvalidate()
 */
void SceneRotate(
    PyMOLGlobals* G, float angle, float x, float y, float z, bool dirty)
{
  CScene *I = G->Scene;
  float temp[16];
  angle = (float) (-PI * angle / 180.0);
  identity44f(temp);
  MatrixRotateC44f(temp, angle, x, y, z);
  MatrixMultiplyC44f(glm::value_ptr(I->m_view.rotMatrix()), temp);
  I->m_view.setRotMatrix(glm::make_mat4(temp));
  SceneUpdateInvMatrix(G);
  if(dirty) {
    SceneInvalidate(G);
  }
}

void SceneRotateAxis(PyMOLGlobals* G, float angle, char axis)
{
  switch (axis) {
  case 'x':
    SceneRotate(G, angle, 1.0f, 0.0f, 0.0f);
    break;
  case 'y':
    SceneRotate(G, angle, 0.0f, 1.0f, 0.0f);
    break;
  case 'z':
    SceneRotate(G, angle, 0.0f, 0.0f, 1.0f);
    break;
  }
}

/*========================================================================*/
void SceneApplyMatrix(PyMOLGlobals * G, float *m)
{
  CScene *I = G->Scene;
  glm::mat4 rot;
  MatrixMultiplyC44f(m, glm::value_ptr(rot));
  I->m_view.setRotMatrix(rot);
  SceneDirty(G);

  /*  glPushMatrix();
     glLoadIdentity();
     glMultMatrixf(m);
     glMultMatrixf(glm::value_ptr(I->m_view.rotMatrix()));
     glGetFloatv(GL_MODELVIEW_MATRIX,glm::value_ptr(I->m_view.rotMatrix()));
     glPopMatrix(); */
}


/*========================================================================*/
void SceneScale(PyMOLGlobals * G, float scale)
{
  CScene *I = G->Scene;
  I->Scale *= scale;
  SceneInvalidate(G);
}

void SceneZoom(PyMOLGlobals * G, float scale){
  CScene *I = G->Scene;
  float factor = -((I->m_view.m_clipSafe().m_front + I->m_view.m_clipSafe().m_back) / 2) * 0.1 * scale;
  /*    SettingGetGlobal_f(G, cSetting_mouse_wheel_scale); */
  I->m_view.translate(0.0f, 0.0f, factor);
  I->m_view.m_clip().m_front -= factor;
  I->m_view.m_clip().m_back -= factor;
  UpdateFrontBackSafe(I);
  SceneInvalidate(G);
}

int SceneGetTwoSidedLighting(PyMOLGlobals * G){
  return SceneGetTwoSidedLightingSettings(G, nullptr, nullptr);
}

int SceneGetTwoSidedLightingSettings(PyMOLGlobals * G,
    const CSetting *set1,
    const CSetting *set2) {
  int two_sided_lighting = SettingGet_b(G, set1, set2, cSetting_two_sided_lighting);
  if(two_sided_lighting<0) {
    if(SettingGet_i(G, set1, set2, cSetting_surface_cavity_mode))
      two_sided_lighting = true;
    else
      two_sided_lighting = false;
  }
  two_sided_lighting = two_sided_lighting || (SettingGet_i(G, set1, set2, cSetting_transparency_mode) ==1);
  return two_sided_lighting;
}

float SceneGetDynamicLineWidth(RenderInfo * info, float line_width)
{
  if(info && info->dynamic_width) {
    float factor;
    if(info->vertex_scale > R_SMALL4) {
      factor = info->dynamic_width_factor / info->vertex_scale;
      if(factor > info->dynamic_width_max)
        factor = info->dynamic_width_max;
      if(factor < info->dynamic_width_min) {
        factor = info->dynamic_width_min;
      }
    } else {
      factor = info->dynamic_width_max;
    }
    return factor * line_width;
  }
  return line_width;
}

float SceneGetLineWidthForCylinders(PyMOLGlobals * G, RenderInfo * info, float line_width_arg){
  float line_width = SceneGetDynamicLineWidth(info, line_width_arg);
 
  float pixel_scale_value = SettingGetGlobal_f(G, cSetting_ray_pixel_scale);
  
  if(pixel_scale_value < 0)
    pixel_scale_value = 1.0F;
  /* the radius of the cylinders is the vertex_scale * ray_pixel_scale */
  /* this turns out to be exactly right, but changes if the scene or user 
     moves */
  return info->vertex_scale * pixel_scale_value * line_width / 2.f;
}

// line width arg has been processed as dynamic already
float SceneGetLineWidthForCylindersStatic(PyMOLGlobals * G, RenderInfo * info, float dynamic_line_width_arg, float line_width_arg){
  float pixel_scale_value = SettingGetGlobal_f(G, cSetting_ray_pixel_scale);  
  if(pixel_scale_value < 0)
    pixel_scale_value = 1.0F;
  /* the radius of the cylinders is the vertex_scale * ray_pixel_scale */
  /* this turns out to be exactly right, but changes if the scene or user 
     moves */
  if (SceneGetStereo(G) == cStereo_openvr) 
    // note: that is reversion of magic dynamic line modifications in PYMOL
    return pixel_scale_value * 0.07f * line_width_arg / 2.0f;
  
  return info->vertex_scale * pixel_scale_value * dynamic_line_width_arg / 2.f;
}

void ScenePushModelViewMatrix(PyMOLGlobals * G) {
  auto I = G->Scene;
  I->m_ModelViewMatrixStack.push_back(I->modelViewMatrix);
}

void ScenePopModelViewMatrix(PyMOLGlobals * G, bool immediate) {
  CScene *I = G->Scene;
  auto& stack = I->m_ModelViewMatrixStack;

  if (stack.empty()) {
    printf("ERROR: depth == 0\n");
    return;
  }

  I->modelViewMatrix = stack.back();
  stack.pop_back();

#ifndef PURE_OPENGL_ES_2
  if (ALWAYS_IMMEDIATE_OR(immediate)) {
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(SceneGetModelViewMatrixPtr(G));
  }
#endif
}

glm::mat4& SceneGetModelViewMatrix(PyMOLGlobals* G) {
  return G->Scene->modelViewMatrix;
}

float* SceneGetModelViewMatrixPtr(PyMOLGlobals* G) {
  auto& mat = SceneGetModelViewMatrix(G);
  return glm::value_ptr(mat);
}

glm::mat4& SceneGetProjectionMatrix(PyMOLGlobals* G) {
  return G->Scene->projectionMatrix;
}

float* SceneGetProjectionMatrixPtr(PyMOLGlobals* G) {
  auto& mat = SceneGetProjectionMatrix(G);
  return glm::value_ptr(mat);
}

void SceneSetBackgroundColorAlreadySet(PyMOLGlobals * G, int background_color_already_set){
  CScene *I = G->Scene;
  I->background_color_already_set = background_color_already_set;
}
int SceneGetBackgroundColorAlreadySet(PyMOLGlobals * G){
  CScene *I = G->Scene;
  return (I->background_color_already_set);
}
void SceneSetDoNotClearBackground(PyMOLGlobals * G, int do_not_clear){
  CScene *I = G->Scene;
  I->do_not_clear = do_not_clear;
}

int SceneGetDoNotClearBackground(PyMOLGlobals * G){
  CScene *I = G->Scene;
  return (I->do_not_clear);
}

void SceneGLClear(PyMOLGlobals * G, GLbitfield mask){
  glClear(mask);
}

int SceneIncrementTextureRefreshes(PyMOLGlobals * G){
  CScene *I = G->Scene;
  return ++(I->n_texture_refreshes);
}

void SceneResetTextureRefreshes(PyMOLGlobals * G){
  CScene *I = G->Scene;
  I->n_texture_refreshes = 0;
}

void SceneGetScaledAxesAtPoint(PyMOLGlobals * G, float *pt, float *xn, float *yn)
{
  CScene *I = G->Scene;
  float xn0[3] = { 1.0F, 0.0F, 0.0F };
  float yn0[3] = { 0.0F, 1.0F, 0.0F };
  float v_scale;

  v_scale = SceneGetScreenVertexScale(G, pt);

  MatrixInvTransformC44fAs33f3f(glm::value_ptr(I->m_view.rotMatrix()), xn0, xn0);
  MatrixInvTransformC44fAs33f3f(glm::value_ptr(I->m_view.rotMatrix()), yn0, yn0);
  scale3f(xn0, v_scale, xn);
  scale3f(yn0, v_scale, yn);
}

void SceneGetScaledAxes(PyMOLGlobals * G, pymol::CObject *obj, float *xn, float *yn)
{
  CScene *I = G->Scene;
  float *v;
  float vt[3];
  float xn0[3] = { 1.0F, 0.0F, 0.0F };
  float yn0[3] = { 0.0F, 1.0F, 0.0F };
  float v_scale;

  v = TextGetPos(G);

  if(obj->TTTFlag) {
    transformTTT44f3f(obj->TTT, v, vt);
  } else {
    copy3f(v, vt);
  }

  v_scale = SceneGetScreenVertexScale(G, vt);

  MatrixInvTransformC44fAs33f3f(glm::value_ptr(I->m_view.rotMatrix()), xn0, xn0);
  MatrixInvTransformC44fAs33f3f(glm::value_ptr(I->m_view.rotMatrix()), yn0, yn0);
  scale3f(xn0, v_scale, xn);
  scale3f(yn0, v_scale, yn);
}

int SceneGetCopyType(PyMOLGlobals * G) {
  return G->Scene->CopyType;
}

void SceneGenerateMatrixToAnotherZFromZ(PyMOLGlobals *G, float *convMatrix, float *curpt, float *pt){
  CScene *I = G->Scene;
  float scaleMatrix[16];
  float cscale = SceneGetExactScreenVertexScale(G, curpt);
  float pscale = SceneGetExactScreenVertexScale(G, pt);
  identity44f(scaleMatrix);
  MatrixSetScaleC44f(scaleMatrix, pscale);
  identity44f(convMatrix);
  MatrixSetScaleC44f(convMatrix, 1.f/cscale);
  MatrixMultiplyC44f(glm::value_ptr(I->m_view.rotMatrix()), convMatrix);
  MatrixTranslateC44f(convMatrix, pt[0]-curpt[0], pt[1]-curpt[1], pt[2]-curpt[2]);
  MatrixMultiplyC44f(I->InvMatrix, convMatrix);
  MatrixMultiplyC44f(scaleMatrix, convMatrix);
}

void SceneAdjustZtoScreenZ(PyMOLGlobals *G, float *pos, float zarg){
  CScene *I = G->Scene;
  float clipRange = (I->m_view.m_clipSafe().m_back-I->m_view.m_clipSafe().m_front);
  float z = (zarg + 1.f) / 2.f;
  float zInPreProj = -(z * clipRange + I->m_view.m_clipSafe().m_front);
  float pos4[4], tpos[4], npos[4];
  float InvModMatrix[16];
  copy3f(pos, pos4);
  pos4[3] = 1.f;
  MatrixTransformC44f4f(SceneGetModelViewMatrixPtr(G), pos4, tpos);
  normalize4f(tpos);
  /* NEED TO ACCOUNT FOR ORTHO */
  if (SettingGetGlobal_b(G, cSetting_ortho)){
    npos[0] = tpos[0];
    npos[1] = tpos[1];
  } else {
    npos[0] = zInPreProj * tpos[0] / tpos[2];
    npos[1] = zInPreProj * tpos[1] / tpos[2];
  }
  npos[2] = zInPreProj;
  npos[3] = 1.f;

  MatrixInvertC44f(SceneGetModelViewMatrixPtr(G), InvModMatrix);
  MatrixTransformC44f4f(InvModMatrix, npos, npos);
  normalize4f(npos);
  copy3f(npos, pos);
}

/* this function takes a screen point, where z is normalized between the clipping 
   planes, and converts it to the world coordinates */
void SceneSetPointToWorldScreenRelative(PyMOLGlobals *G, float *pos, float *screenPt)
{
  float npos[4];
  float InvPmvMatrix[16];
  auto extent = SceneGetExtentStereo(G);
  npos[0] = (.5f + floor(screenPt[0] * extent.width)) /
            extent.width; // add .5, in middle of pixels?
  npos[1] = (.5f + floor(screenPt[1] * extent.height)) /
            extent.height; // add .5, in middle of pixels?
  npos[2] = 0.f;
  npos[3] = 1.f;
  MatrixInvertC44f(SceneGetPmvMatrix(G), InvPmvMatrix);
  MatrixTransformC44f4f(InvPmvMatrix, npos, npos);
  normalize4f(npos);
  SceneAdjustZtoScreenZ(G, npos, screenPt[2]);
  copy3f(npos, pos);
}

float SceneGetCurrentBackSafe(PyMOLGlobals *G){
  CScene *I = G->Scene;
  return (I->m_view.m_clipSafe().m_back);
}
float SceneGetCurrentFrontSafe(PyMOLGlobals *G){
  CScene *I = G->Scene;
  return (I->m_view.m_clipSafe().m_front);
}

/**
 * Get the field-of-view width at a depth of 1.0
 */
float GetFovWidth(PyMOLGlobals * G)
{
  float fov = SettingGetGlobal_f(G, cSetting_field_of_view);
  return 2.f * tanf(fov * PI / 360.f);
}

void SceneInvalidatePicking(PyMOLGlobals * G){
  CScene *I = G->Scene;
  I->pickmgr.invalidate();
}

float SceneGetScale(PyMOLGlobals * G) {
  return G->Scene->Scale;
}

void ScenePickAtomInWorld(PyMOLGlobals * G, int x, int y, float *atomWorldPos) {
  CScene *I = G->Scene;
  if (SceneDoXYPick(G, x, y, ClickSide::None)) {
    pymol::CObject *obj = I->LastPicked.context.object;
    if (obj->type != cObjectMolecule) {
      return;
    }
    // get atom pos in Local CS
    float atomPos[3];
    ObjectMoleculeGetAtomTxfVertex((ObjectMolecule *)I->LastPicked.context.object, 0, I->LastPicked.src.index, atomPos);
    // muptiply by molecule world matrix
    MatrixTransformC44f3f(SceneGetModelViewMatrixPtr(G), atomPos, atomWorldPos);
  }
}

std::shared_ptr<pymol::Image> SceneGetSharedImage(PyMOLGlobals* G)
{
  return G->Scene->Image;
}

void CScene::setSceneView(const SceneView& view) {
  m_view.setView(view);
  SceneInvalidate(m_G);
}
SceneElem::SceneElem(std::string name_, bool drawn_)
  : name(std::move(name_))
  , drawn(drawn_)
{
}

void SceneSetViewport(PyMOLGlobals* G, const Rect2D& rect)
{
  switch (G->GFXMgr->backend()) {
  case GFXAPIBackend::OPENGL:
    glViewport(rect.offset.x, rect.offset.y, rect.extent.width, rect.extent.height);
  default:
    break;
  }
}

void SceneSetViewport(PyMOLGlobals* G, int x, int y, int width, int height)
{
  assert(width >= 0 && height >= 0);
  SceneSetViewport(G, Rect2D{x, y, static_cast<std::uint32_t>(width),
                          static_cast<std::uint32_t>(height)});
}

Rect2D SceneGetViewport(PyMOLGlobals* G)
{
  Rect2D viewport{};
  int viewBuffer[4];
  glGetIntegerv(GL_VIEWPORT, (GLint*) (void*) viewBuffer);
  viewport.offset = Offset2D{static_cast<std::int32_t>(viewBuffer[0]),
      static_cast<std::int32_t>(viewBuffer[1])};
  viewport.extent = Extent2D{static_cast<std::uint32_t>(viewBuffer[2]),
      static_cast<std::uint32_t>(viewBuffer[3])};
return viewport;
}
