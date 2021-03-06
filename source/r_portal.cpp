// Emacs style mode select   -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// Copyright(C) 2013 Stephen McGranahan et al.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/
//
// Additional terms and conditions compatible with the GPLv3 apply. See the
// file COPYING-EE for details.
//
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//      Creating, managing, and rendering portals.
//      SoM created 12/8/03
//
//-----------------------------------------------------------------------------

#include "z_zone.h"
#include "i_system.h"

#include "c_io.h"
#include "r_bsp.h"
#include "r_draw.h"
#include "r_main.h"
#include "r_plane.h"
#include "r_portal.h"
#include "r_state.h"
#include "r_things.h"
#include "v_alloc.h"
#include "v_misc.h"

//=============================================================================
//
// Portal Spawning and Management
//

static portal_t *portals = NULL, *last = NULL;
static pwindow_t *unusedhead = NULL, *windowhead = NULL, *windowlast = NULL;

//
// VALLOCATION(portals)
//
// haleyjd 04/30/13: when the resolution changes, all portals need notification.
//
VALLOCATION(portals)
{
   for(portal_t *p = portals; p; p = p->next)
   {
      planehash_t *hash;

      // clear portal overlay visplane hash tables
      if((hash = p->poverlay))
      {
         for(int i = 0; i < hash->chaincount; i++)
            hash->chains[i] = NULL;
      }
   }

   // free portal window structures on the main list
   pwindow_t *rover = windowhead;
   while(rover)
   {
      pwindow_t *child = rover->child;
      pwindow_t *next;

      // free any child windows
      while(child)
      {
         next = child->child;
         efree(child->top);
         efree(child);
         child = next;
      }

      // free this window
      next = rover->next;
      efree(rover->top);
      efree(rover);
      rover = next;
   }

   // free portal window structures on the freelist
   rover = unusedhead;
   while(rover)
   {
      pwindow_t *next = rover->next;
      efree(rover->top);
      efree(rover);
      rover = next;
   }

   windowhead = windowlast = unusedhead = NULL;   
}

// This flag is set when a portal is being rendered. This flag is checked in 
// r_bsp.c when rendering camera portals (skybox, anchored, linked) so that an
// extra function (R_ClipSegToPortal) is called to prevent certain types of HOM
// in portals.

portalrender_t portalrender = { false, MAX_SCREENWIDTH, 0 };

static void R_RenderPortalNOP(pwindow_t *window)
{
   I_Error("R_RenderPortalNOP called\n");
}

static void R_SetPortalFunction(pwindow_t *window);

static void R_ClearPortalWindow(pwindow_t *window)
{
   window->maxx = 0;
   window->minx = viewwindow.width - 1;

   for(int i = 0; i < video.width; i++)
   {
      window->top[i]    = view.height;
      window->bottom[i] = -1.0f;
   }

   window->child    = NULL;
   window->next     = NULL;
   window->portal   = NULL;
   window->line     = NULL;
   window->func     = R_RenderPortalNOP;
   window->clipfunc = NULL;
   window->vx = window->vy = window->vz = 0;
}

static pwindow_t *newPortalWindow()
{
   pwindow_t *ret;

   if(unusedhead)
   {
      ret = unusedhead;
      unusedhead = unusedhead->next;
   }
   else
   {
      ret = estructalloctag(pwindow_t, 1, PU_LEVEL);
      
      float *buf  = emalloctag(float *, 2*video.width*sizeof(float), PU_LEVEL, NULL);
      ret->top    = buf;
      ret->bottom = buf + video.width;
   }

   R_ClearPortalWindow(ret);
   
   return ret;
}

static pwindow_t *R_NewPortalWindow(portal_t *p, line_t *l, pwindowtype_e type)
{
   pwindow_t *ret = newPortalWindow();
   
   ret->portal = p;
   ret->line   = l;
   ret->type   = type;
   ret->head   = ret;
   
   R_SetPortalFunction(ret);
   
   if(!windowhead)
      windowhead = windowlast = ret;
   else
   {
      windowlast->next = ret;
      windowlast = ret;
   }
   
   return ret;
}

//
// R_CreateChildWindow
//
// Spawns a child portal for an existing portal. Each portal can only
// have one child.
//
static void R_CreateChildWindow(pwindow_t *parent)
{
#ifdef RANGECHECK
   if(parent->child)
      I_Error("R_CreateChildWindow: child portal displaced\n");
#endif

   auto child = newPortalWindow();

   parent->child   = child;
   child->head     = parent->head;
   child->portal   = parent->portal;
   child->line     = parent->line;
   child->type     = parent->type;
   child->func     = parent->func;
   child->clipfunc = parent->clipfunc;
}

//
// R_WindowAdd
//
// Adds a column to a portal for rendering. A child portal may
// be created.
//
void R_WindowAdd(pwindow_t *window, int x, float ytop, float ybottom)
{
   float windowtop, windowbottom;

#ifdef RANGECHECK
   if(!window)
      I_Error("R_WindowAdd: null portal window\n");

   if(x < 0 || x >= video.width)
      I_Error("R_WindowAdd: column out of bounds (%d)\n", x);

   if((ybottom >= view.height || ytop < 0) && ytop < ybottom)
   {
      I_Error("R_WindowAdd portal supplied with bad column data.\n"
              "\tx:%d, top:%f, bottom:%f\n", x, ytop, ybottom);
   }
#endif

   windowtop    = window->top[x];
   windowbottom = window->bottom[x];

#ifdef RANGECHECK
   if(windowbottom > windowtop && 
      (windowtop < 0 || windowbottom < 0 || 
       windowtop >= view.height || windowbottom >= view.height))
   {
      I_Error("R_WindowAdd portal had bad opening data.\n"
              "\tx:%i, top:%f, bottom:%f\n", x, windowtop, windowbottom);
   }
#endif

   if(ybottom < 0.0f || ytop >= view.height)
      return;

   if(x <= window->maxx && x >= window->minx)
   {
      // column falls inside the range of the portal.

      // check to see if the portal column isn't occupied
      if(windowtop > windowbottom)
      {
         window->top[x]    = ytop;
         window->bottom[x] = ybottom;
         return;
      }

      // if the column lays completely outside the existing portal, create child
      if(ytop > windowbottom || ybottom < windowtop)
      {
         if(!window->child)
            R_CreateChildWindow(window);

         R_WindowAdd(window->child, x, ytop, ybottom);
         return;
      }

      // because a check has already been made to reject the column, the columns
      // must intersect; expand as needed
      if(ytop < windowtop)
         window->top[x] = ytop;

      if(ybottom > windowbottom)
         window->bottom[x] = ybottom;
      return;
   }

   if(window->minx > window->maxx)
   {
      // Portal is empty so place the column anywhere (first column added to 
      // the portal)
      window->minx = window->maxx = x;
      window->top[x]    = ytop;
      window->bottom[x] = ybottom;

      // SoM 3/10/2005: store the viewz in the portal struct for later use
      window->vx = viewx;
      window->vy = viewy;
      window->vz = viewz;
      return;
   }

   if(x > window->maxx)
   {
      window->maxx = x;

      window->top[x]    = ytop;
      window->bottom[x] = ybottom;
      return;
   }

   if(x < window->minx)
   {
      window->minx = x;

      window->top[x]    = ytop;
      window->bottom[x] = ybottom;
      return;
   }
}

//
// R_CreatePortal
//
// Function to internally create a new portal.
//
static portal_t *R_CreatePortal()
{
   portal_t *ret = estructalloctag(portal_t, 1, PU_LEVEL);

   if(!portals)
      portals = last = ret;
   else
   {
      last->next = ret;
      last = ret;
   }
   
   ret->poverlay  = R_NewPlaneHash(32);
   ret->globaltex = 1;

   return ret;
}

//
// R_CalculateDeltas
//
// Calculates the deltas (offset) between two linedefs.
//
static void R_CalculateDeltas(int markerlinenum, int anchorlinenum, 
                              fixed_t *dx, fixed_t *dy, fixed_t *dz)
{
   line_t *m = lines + markerlinenum;
   line_t *a = lines + anchorlinenum;

   *dx = ((m->v1->x + m->v2->x) / 2) - ((a->v1->x + a->v2->x) / 2);
   *dy = ((m->v1->y + m->v2->y) / 2) - ((a->v1->y + a->v2->y) / 2);
   *dz = 0; /// ???
}

//
// R_GetAnchoredPortal
//
// Either finds a matching existing anchored portal matching the
// parameters, or creates a new one. Used in p_spec.c.
//
portal_t *R_GetAnchoredPortal(int markerlinenum, int anchorlinenum)
{
   portal_t *rover, *ret;
   edefstructvar(anchordata_t, adata);

   R_CalculateDeltas(markerlinenum, anchorlinenum, 
                     &adata.deltax, &adata.deltay, &adata.deltaz);

   adata.maker = markerlinenum;
   adata.anchor = anchorlinenum;

   for(rover = portals; rover; rover = rover->next)
   {
      if(rover->type != R_ANCHORED || 
         adata.deltax != rover->data.anchor.deltax ||
         adata.deltay != rover->data.anchor.deltay ||
         adata.deltaz != rover->data.anchor.deltaz)
         continue;

      return rover;
   }

   ret = R_CreatePortal();
   ret->type = R_ANCHORED;
   ret->data.anchor = adata;

   // haleyjd: temporary debug
   ret->tainted = 0;

   return ret;
}

//
// R_GetTwoWayPortal
//
// Either finds a matching existing two-way anchored portal matching the
// parameters, or creates a new one. Used in p_spec.c.
//
portal_t *R_GetTwoWayPortal(int markerlinenum, int anchorlinenum)
{
   portal_t *rover, *ret;
   edefstructvar(anchordata_t, adata);

   R_CalculateDeltas(markerlinenum, anchorlinenum, 
                     &adata.deltax, &adata.deltay, &adata.deltaz);

   adata.maker = markerlinenum;
   adata.anchor = anchorlinenum;

   for(rover = portals; rover; rover = rover->next)
   {
      if(rover->type  != R_TWOWAY                  || 
         adata.deltax != rover->data.anchor.deltax ||
         adata.deltay != rover->data.anchor.deltay ||
         adata.deltaz != rover->data.anchor.deltaz)
         continue;

      return rover;
   }

   ret = R_CreatePortal();
   ret->type = R_TWOWAY;
   ret->data.anchor = adata;

   // haleyjd: temporary debug
   ret->tainted = 0;

   return ret;
}

//
// R_GetSkyBoxPortal
//
// Either finds a portal for the provided camera object, or creates
// a new one for it. Used in p_spec.c.
//
portal_t *R_GetSkyBoxPortal(Mobj *camera)
{
   portal_t *rover, *ret;

   for(rover = portals; rover; rover = rover->next)
   {
      if(rover->type != R_SKYBOX || rover->data.camera != camera)
         continue;

      return rover;
   }

   ret = R_CreatePortal();
   ret->type = R_SKYBOX;
   ret->data.camera = camera;
   return ret;
}

//
// R_GetHorizonPortal
//
// Either finds an existing horizon portal matching the parameters,
// or creates a new one. Used in p_spec.c.
//
portal_t *R_GetHorizonPortal(int *floorpic, int *ceilingpic, 
                             fixed_t *floorz, fixed_t *ceilingz, 
                             int16_t *floorlight, int16_t *ceilinglight, 
                             fixed_t *floorxoff, fixed_t *flooryoff, 
                             fixed_t *ceilingxoff, fixed_t *ceilingyoff,
                             float *floorbaseangle, float *floorangle,
                             float *ceilingbaseangle, float *ceilingangle)
{
   portal_t *rover, *ret;
   edefstructvar(horizondata_t, horizon);

   if(!floorpic || !ceilingpic || !floorz || !ceilingz || 
      !floorlight || !ceilinglight || !floorxoff || !flooryoff || 
      !ceilingxoff || !ceilingyoff || !floorbaseangle || !floorangle ||
      !ceilingbaseangle || !ceilingangle)
      return NULL;

   horizon.ceilinglight     = ceilinglight;
   horizon.floorlight       = floorlight;
   horizon.ceilingpic       = ceilingpic;
   horizon.floorpic         = floorpic;
   horizon.ceilingz         = ceilingz;
   horizon.floorz           = floorz;
   horizon.ceilingxoff      = ceilingxoff;
   horizon.ceilingyoff      = ceilingyoff;
   horizon.floorxoff        = floorxoff;
   horizon.flooryoff        = flooryoff;
   horizon.floorbaseangle   = floorbaseangle; // haleyjd 01/05/08
   horizon.floorangle       = floorangle;
   horizon.ceilingbaseangle = ceilingbaseangle;
   horizon.ceilingangle     = ceilingangle;

   for(rover = portals; rover; rover = rover->next)
   {
      if(rover->type != R_HORIZON || 
         memcmp(&rover->data.horizon, &horizon, sizeof(horizon)))
         continue;

      return rover;
   }

   ret = R_CreatePortal();
   ret->type = R_HORIZON;
   ret->data.horizon = horizon;
   return ret;
}

//
// R_GetPlanePortal
//
// Either finds a plane portal matching the parameters, or creates a
// new one. Used in p_spec.c.
//
portal_t *R_GetPlanePortal(int *pic, fixed_t *delta, 
                           int16_t *lightlevel, 
                           fixed_t *xoff, fixed_t *yoff,
                           float *baseangle, float *angle)
{
   portal_t *rover, *ret;
   edefstructvar(skyplanedata_t, skyplane);

   if(!pic || !delta || !lightlevel || !xoff || !yoff || !baseangle || !angle)
      return NULL;
      
   skyplane.pic        = pic;
   skyplane.delta      = delta;
   skyplane.lightlevel = lightlevel;
   skyplane.xoff       = xoff;
   skyplane.yoff       = yoff;
   skyplane.baseangle  = baseangle; // haleyjd 01/05/08: flat angles
   skyplane.angle      = angle;    

   for(rover = portals; rover; rover = rover->next)
   {
      if(rover->type != R_PLANE || 
         memcmp(&rover->data.plane, &skyplane, sizeof(skyplane)))
         continue;

      return rover;
   }

   ret = R_CreatePortal();
   ret->type = R_PLANE;
   ret->data.plane = skyplane;
   return ret;
}

//
// R_InitPortals
//
// Called before P_SetupLevel to reset the portal list.
// Portals are allocated at PU_LEVEL cache level, so they'll
// be implicitly freed.
//
void R_InitPortals()
{
   portals = last = NULL;
   windowhead = unusedhead = windowlast = NULL;
}

//=============================================================================
//
// Plane and Horizon Portals
//

//
// R_RenderPlanePortal
//
static void R_RenderPlanePortal(pwindow_t *window)
{
   visplane_t *vplane;
   int x;
   float angle;
   portal_t *portal = window->portal;

   portalrender.curwindow = window;

   if(portal->type != R_PLANE)
      return;

   if(window->maxx < window->minx)
      return;

   // haleyjd 01/05/08: flat angle
   angle = *portal->data.plane.baseangle + *portal->data.plane.angle;

   vplane = R_FindPlane(*portal->data.plane.delta + viewz, 
                        *portal->data.plane.pic, 
                        *portal->data.plane.lightlevel, 
                        *portal->data.plane.xoff, 
                        *portal->data.plane.yoff,
                        angle, NULL, 0, 255, NULL);

   vplane = R_CheckPlane(vplane, window->minx, window->maxx);

   for(x = window->minx; x <= window->maxx; x++)
   {
      if(window->top[x] < window->bottom[x])
      {
         vplane->top[x]    = (int)window->top[x];
         vplane->bottom[x] = (int)window->bottom[x];
      }
   }

   if(window->head == window && window->portal->poverlay)
      R_PushPost(false, window->portal->poverlay);
      
   if(window->child)
      R_RenderPlanePortal(window->child);
}

//
// R_RenderHorizonPortal
//
static void R_RenderHorizonPortal(pwindow_t *window)
{
   fixed_t lastx, lasty, lastz; // SoM 3/10/2005
   float   lastxf, lastyf, lastzf, floorangle, ceilingangle;
   visplane_t *topplane, *bottomplane;
   int x;
   portal_t *portal = window->portal;

   portalrender.curwindow = window;

   if(portal->type != R_HORIZON)
      return;

   if(window->maxx < window->minx)
      return;

   // haleyjd 01/05/08: angles
   floorangle = *portal->data.horizon.floorbaseangle + 
                *portal->data.horizon.floorangle;

   ceilingangle = *portal->data.horizon.ceilingbaseangle +
                  *portal->data.horizon.ceilingangle;

   topplane = R_FindPlane(*portal->data.horizon.ceilingz, 
                          *portal->data.horizon.ceilingpic, 
                          *portal->data.horizon.ceilinglight, 
                          *portal->data.horizon.ceilingxoff, 
                          *portal->data.horizon.ceilingyoff,
                          ceilingangle, NULL, 0, 255, NULL);

   bottomplane = R_FindPlane(*portal->data.horizon.floorz, 
                             *portal->data.horizon.floorpic, 
                             *portal->data.horizon.floorlight, 
                             *portal->data.horizon.floorxoff, 
                             *portal->data.horizon.flooryoff,
                             floorangle, NULL, 0, 255, NULL);

   topplane = R_CheckPlane(topplane, window->minx, window->maxx);
   bottomplane = R_CheckPlane(bottomplane, window->minx, window->maxx);

   for(x = window->minx; x <= window->maxx; x++)
   {
      if(window->top[x] > window->bottom[x])
         continue;

      if(window->top[x]    <= view.ycenter - 1.0f && 
         window->bottom[x] >= view.ycenter)
      {
         topplane->top[x]       = (int)window->top[x];
         topplane->bottom[x]    = centery - 1;
         bottomplane->top[x]    = centery;
         bottomplane->bottom[x] = (int)window->bottom[x];
      }
      else if(window->top[x] <= view.ycenter - 1.0f)
      {
         topplane->top[x]    = (int)window->top[x];
         topplane->bottom[x] = (int)window->bottom[x];
      }
      else if(window->bottom[x] > view.ycenter - 1.0f)
      {
         bottomplane->top[x]    = (int)window->top[x];
         bottomplane->bottom[x] = (int)window->bottom[x];
      }
   }

   lastx  = viewx; 
   lasty  = viewy; 
   lastz  = viewz;
   lastxf = view.x;
   lastyf = view.y;
   lastzf = view.z;

   viewx = window->vx;   
   viewy = window->vy;   
   viewz = window->vz;   
   view.x = M_FixedToFloat(viewx);
   view.y = M_FixedToFloat(viewy);
   view.z = M_FixedToFloat(viewz);

   if(window->head == window && window->portal->poverlay)
      R_PushPost(false, window->portal->poverlay);
      
   if(window->child)
      R_RenderHorizonPortal(window->child);

   viewx  = lastx; 
   viewy  = lasty; 
   viewz  = lastz;
   view.x = lastxf;
   view.y = lastyf;
   view.z = lastzf;
}

//=============================================================================
//
// Skybox Portals
//

extern void R_ClearSlopeMark(int minx, int maxx, pwindowtype_e type);

//
// R_RenderSkyboxPortal
//
static void R_RenderSkyboxPortal(pwindow_t *window)
{
   fixed_t lastx, lasty, lastz, lastangle;
   float   lastxf, lastyf, lastzf, lastanglef;
   portal_t *portal = window->portal;

   portalrender.curwindow = window;

   if(portal->type != R_SKYBOX)
      return;

   if(window->maxx < window->minx)
      return;

#ifdef RANGECHECK
   for(int i = 0; i < video.width; i++)
   {
      if(window->bottom[i] > window->top[i] && (window->bottom[i] < -1 
         || window->bottom[i] > viewwindow.height || window->top[i] < -1 
         || window->top[i] > viewwindow.height))
      {
         I_Error("R_RenderSkyboxPortal: clipping array contained invalid "
                 "information:\n"
                 "   x:%i, ytop:%f, ybottom:%f\n", 
                 i, window->top[i], window->bottom[i]);
      }
   }
#endif

   if(!R_SetupPortalClipsegs(window->minx, window->maxx, window->top, window->bottom))
      return;

   R_ClearSlopeMark(window->minx, window->maxx, window->type);

   floorclip   = window->bottom;
   ceilingclip = window->top;
   
   R_ClearOverlayClips();

   portalrender.minx = window->minx;
   portalrender.maxx = window->maxx;

   ++validcount;
   R_SetMaskedSilhouette(ceilingclip, floorclip);

   lastx = viewx;
   lasty = viewy;
   lastz = viewz;
   lastangle = viewangle;
   lastxf = view.x;
   lastyf = view.y;
   lastzf = view.z;
   lastanglef = view.angle;

   viewx = portal->data.camera->x;
   viewy = portal->data.camera->y;
   viewz = portal->data.camera->z;
   view.x = M_FixedToFloat(viewx);
   view.y = M_FixedToFloat(viewy);
   view.z = M_FixedToFloat(viewz);

   // SoM: The viewangle should also be offset by the skybox camera angle.
   viewangle += portal->data.camera->angle;
   viewsin = finesine[viewangle>>ANGLETOFINESHIFT];
   viewcos = finecosine[viewangle>>ANGLETOFINESHIFT];

   view.angle = (ANG90 - viewangle) * PI / ANG180;
   view.sin = (float)sin(view.angle);
   view.cos = (float)cos(view.angle);

   R_IncrementFrameid();
   R_RenderBSPNode(numnodes - 1);
   
   // Only push the overlay if this is the head window
   R_PushPost(true, window->head == window ? window->portal->poverlay : NULL);

   floorclip   = floorcliparray;
   ceilingclip = ceilingcliparray;

   // SoM: "pop" the view state.
   viewx = lastx;
   viewy = lasty;
   viewz = lastz;
   viewangle = lastangle;
   view.x = lastxf;
   view.y = lastyf;
   view.z = lastzf;
   view.angle = lastanglef;

   viewsin  = finesine[viewangle>>ANGLETOFINESHIFT];
   viewcos  = finecosine[viewangle>>ANGLETOFINESHIFT];
   view.sin = (float)sin(view.angle);
   view.cos = (float)cos(view.angle);

   if(window->child)
      R_RenderSkyboxPortal(window->child);
}

//=============================================================================
//
// Anchored and Linked Portals
//

extern int    showtainted;

static void R_ShowTainted(pwindow_t *window)
{
   static byte taintcolor = 0;
   int y1, y2, count;

   for(int i = window->minx; i <= window->maxx; i++)
   {
      byte *dest;

      y1 = (int)window->top[i];
      y2 = (int)window->bottom[i];

      count = y2 - y1 + 1;
      if(count <= 0)
         continue;

      dest = R_ADDRESS(i, y1);

      while(count > 0)
      {
         *dest = taintcolor;
         dest += video.pitch;

         count--;
      }
   }
   taintcolor += 16;
}

//
// R_RenderAnchoredPortal
//
static void R_RenderAnchoredPortal(pwindow_t *window)
{
   fixed_t lastx, lasty, lastz;
   float   lastxf, lastyf, lastzf;
   portal_t *portal = window->portal;

   // ioanch 20160123: don't forget
   portalrender.curwindow = window;

   if(portal->type != R_ANCHORED && portal->type != R_TWOWAY)
      return;

   if(window->maxx < window->minx)
      return;

   // haleyjd: temporary debug
   if(portal->tainted > 6)
   {
      if(showtainted)
         R_ShowTainted(window);         

      portal->tainted++;
      C_Printf(FC_ERROR "Refused to draw portal (line=%i) (t=%d)\n", 
               portal->data.anchor.maker, portal->tainted);
      return;
   } 

#ifdef RANGECHECK
   for(int i = 0; i < video.width; i++)
   {
      if(window->bottom[i] > window->top[i] && (window->bottom[i] < -1 
         || window->bottom[i] > viewwindow.height || window->top[i] < -1 
         || window->top[i] > viewwindow.height))
      {
         I_Error("R_RenderAnchoredPortal: clipping array contained invalid "
                 "information:\n" 
                 "   x:%i, ytop:%f, ybottom:%f\n", 
                 i, window->top[i], window->bottom[i]);
      }
   }
#endif
   
   if(!R_SetupPortalClipsegs(window->minx, window->maxx, window->top, window->bottom))
      return;

   R_ClearSlopeMark(window->minx, window->maxx, window->type);

   // haleyjd: temporary debug
   portal->tainted++;

   floorclip   = window->bottom;
   ceilingclip = window->top;

   R_ClearOverlayClips();
   
   portalrender.minx = window->minx;
   portalrender.maxx = window->maxx;

   ++validcount;
   R_SetMaskedSilhouette(ceilingclip, floorclip);

   lastx = viewx;
   lasty = viewy;
   lastz = viewz;
   lastxf = view.x;
   lastyf = view.y;
   lastzf = view.z;


   // SoM 3/10/2005: Use the coordinates stored in the portal struct
   viewx  = window->vx + portal->data.anchor.deltax;
   viewy  = window->vy + portal->data.anchor.deltay;
   viewz  = window->vz + portal->data.anchor.deltaz;
   view.x = M_FixedToFloat(viewx);
   view.y = M_FixedToFloat(viewy);
   view.z = M_FixedToFloat(viewz);

   R_IncrementFrameid();
   R_RenderBSPNode(numnodes - 1);

   // Only push the overlay if this is the head window
   R_PushPost(true, window->head == window ? window->portal->poverlay : NULL);

   floorclip = floorcliparray;
   ceilingclip = ceilingcliparray;

   viewx  = lastx;
   viewy  = lasty;
   viewz  = lastz;
   view.x = lastxf;
   view.y = lastyf;
   view.z = lastzf;

   if(window->child)
      R_RenderAnchoredPortal(window->child);
}

static void R_RenderLinkedPortal(pwindow_t *window)
{
   fixed_t lastx, lasty, lastz;
   float   lastxf, lastyf, lastzf;
   portal_t *portal = window->portal;

   // ioanch 20160123: keep track of window
   portalrender.curwindow = window;

   if(portal->type != R_LINKED || window->maxx < window->minx)
      return;

   // haleyjd: temporary debug
   if(portal->tainted > 6)
   {
      if(showtainted)
         R_ShowTainted(window);         

      portal->tainted++;
      C_Printf(FC_ERROR "Refused to draw portal (line=%i) (t=%d)", 
               portal->data.link.maker, portal->tainted);
      return;
   } 

#ifdef RANGECHECK
   for(int i = 0; i < video.width; i++)
   {
      if(window->bottom[i] > window->top[i] && (window->bottom[i] < -1 
         || window->bottom[i] > viewwindow.height || window->top[i] < -1 
         || window->top[i] > viewwindow.height))
      {
         I_Error("R_RenderAnchoredPortal: clipping array contained invalid "
                 "information:\n" 
                 "   x:%i, ytop:%f, ybottom:%f\n", 
                 i, window->top[i], window->bottom[i]);
      }
   }
#endif
   
   if(!R_SetupPortalClipsegs(window->minx, window->maxx, window->top, window->bottom))
      return;

   R_ClearSlopeMark(window->minx, window->maxx, window->type);

   // haleyjd: temporary debug
   portal->tainted++;

   floorclip   = window->bottom;
   ceilingclip = window->top;

   R_ClearOverlayClips();
   
   portalrender.minx = window->minx;
   portalrender.maxx = window->maxx;

   ++validcount;
   R_SetMaskedSilhouette(ceilingclip, floorclip);

   lastx  = viewx;
   lasty  = viewy;
   lastz  = viewz;
   lastxf = view.x;
   lastyf = view.y;
   lastzf = view.z;

   // SoM 3/10/2005: Use the coordinates stored in the portal struct
   viewx  = window->vx + portal->data.link.deltax;
   viewy  = window->vy + portal->data.link.deltay;
   viewz  = window->vz + portal->data.link.deltaz;
   view.x = M_FixedToFloat(viewx);
   view.y = M_FixedToFloat(viewy);
   view.z = M_FixedToFloat(viewz);

   R_IncrementFrameid();
   R_RenderBSPNode(numnodes - 1);

   // Only push the overlay if this is the head window
   R_PushPost(true, window->head == window ? window->portal->poverlay : NULL);

   floorclip = floorcliparray;
   ceilingclip = ceilingcliparray;

   viewx  = lastx;
   viewy  = lasty;
   viewz  = lastz;
   view.x = lastxf;
   view.y = lastyf;
   view.z = lastzf;

   if(window->child)
      R_RenderLinkedPortal(window->child);
}

//
// R_UntaintPortals
//
// haleyjd: temporary debug (maybe)
// Clears the tainted count for all portals to zero.
// This allows the renderer to keep track of how many times a portal has been
// rendered during a frame. If that count exceeds a given limit (which is
// currently somewhat arbitrarily set to the screen width), the renderer will
// refuse to render the portal any more during that frame. This prevents run-
// away recursion between multiple portals, as well as run-away recursion into
// the same portal due to floor/ceiling overlap caused by using non-two-way
// anchored portals in two-way situations. Only anchored portals and skyboxes
// are susceptible to this problem.
//
void R_UntaintPortals()
{
   portal_t *r;

   for(r = portals; r; r = r->next)
   {
      r->tainted = 0;
   }
}

static void R_SetPortalFunction(pwindow_t *window)
{
   switch(window->portal->type)
   {
   case R_PLANE:
      window->func     = R_RenderPlanePortal;
      window->clipfunc = NULL;
      break;
   case R_HORIZON:
      window->func     = R_RenderHorizonPortal;
      window->clipfunc = NULL;
      break;
   case R_SKYBOX:
      window->func     = R_RenderSkyboxPortal;
      window->clipfunc = NULL;
      break;
   case R_ANCHORED:
   case R_TWOWAY:
      window->func     = R_RenderAnchoredPortal;
      window->clipfunc = segclipfuncs[window->type];
      break;
   case R_LINKED:
      window->func     = R_RenderLinkedPortal;
      window->clipfunc = segclipfuncs[window->type];
      break;
   default:
      window->func     = R_RenderPortalNOP;
      window->clipfunc = NULL;
      break;
   }
}

//
// R_Get*PortalWindow
//
// functions return a portal window based on the given parameters.
//
pwindow_t *R_GetFloorPortalWindow(portal_t *portal)
{
   pwindow_t *rover = windowhead;

   while(rover)
   {
      // SoM: TODO: There could be the possibility of multiple portals
      // being able to share a single window set.
      if(rover->portal == portal && rover->type == pw_floor)
         return rover;
   
      rover = rover->next;
   }

   // not found, so make it
   return R_NewPortalWindow(portal, NULL, pw_floor);
}

pwindow_t *R_GetCeilingPortalWindow(portal_t *portal)
{
   pwindow_t *rover = windowhead;

   while(rover)
   {
      if(rover->portal == portal && rover->type == pw_ceiling)
         return rover;

      rover = rover->next;
   }

   // not found, so make it
   return R_NewPortalWindow(portal, NULL, pw_ceiling);
}

pwindow_t *R_GetLinePortalWindow(portal_t *portal, line_t *line)
{
   pwindow_t *rover = windowhead;

   while(rover)
   {
      if(rover->portal == portal && rover->type == pw_line && 
         rover->line == line)
         return rover;

      rover = rover->next;
   }

   // not found, so make it
   return R_NewPortalWindow(portal, line, pw_line);
}

//
// R_ClearPortals
//
// Called at the start of each frame
//
void R_ClearPortals()
{
   portal_t *r = portals;
   
   while(r)
   {
      R_ClearPlaneHash(r->poverlay);
      r = r->next;
   }
}

//
// R_RenderPortals
//
// Primary portal rendering function.
//
void R_RenderPortals()
{
   pwindow_t *w;

   while(windowhead)
   {
      portalrender.active = true;
      portalrender.w = windowhead;
      portalrender.segClipFunc = windowhead->clipfunc;
      portalrender.overlay = windowhead->portal->poverlay;
      portalrender.curwindow = windowhead;   // ioanch 20160123: for safety

      if(windowhead->maxx >= windowhead->minx)
         windowhead->func(windowhead);

      portalrender.active = false;
      portalrender.w = NULL;
      portalrender.segClipFunc = NULL;
      portalrender.overlay = NULL;
      portalrender.curwindow = nullptr;   // ioanch 20160123: reset it

      // free the window structs
      w = windowhead->child;
      while(w)
      {
         w->next = unusedhead;
         unusedhead = w;
         w = w->child;
         unusedhead->child = NULL;
      }

      w = windowhead->next;
      windowhead->next = unusedhead;
      unusedhead = windowhead;
      unusedhead->child = NULL;

      windowhead = w;
   }

   windowlast = windowhead;
}

//=============================================================================
//
// SoM: Begin linked portals
//

portal_t *R_GetLinkedPortal(int markerlinenum, int anchorlinenum, 
                            fixed_t planez,    int fromid,
                            int toid)
{
   portal_t *rover, *ret;
   edefstructvar(linkdata_t, ldata);

   ldata.fromid = fromid;
   ldata.toid   = toid;
   ldata.planez = planez;

   R_CalculateDeltas(markerlinenum, anchorlinenum, 
                     &ldata.deltax, &ldata.deltay, &ldata.deltaz);

   ldata.maker = markerlinenum;
   ldata.anchor = anchorlinenum;

   for(rover = portals; rover; rover = rover->next)
   {
      if(rover->type  != R_LINKED                || 
         ldata.deltax != rover->data.link.deltax ||
         ldata.deltay != rover->data.link.deltay ||
         ldata.deltaz != rover->data.link.deltaz ||
         ldata.fromid != rover->data.link.fromid ||
         ldata.toid   != rover->data.link.toid   ||
         ldata.planez != rover->data.link.planez)
         continue;

      return rover;
   }

   ret = R_CreatePortal();
   ret->type = R_LINKED;
   ret->data.link = ldata;

   // haleyjd: temporary debug
   ret->tainted = 0;

   return ret;
}

// EOF

