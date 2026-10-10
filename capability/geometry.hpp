#pragma once
#include <stdint.h>
/* The original renderers use a 960x540 landscape design canvas. Fit that
 * canvas, preserving aspect ratio, into the reported physical display. */
struct CapGeometry {
  unsigned raw_width,raw_height,width,height,x,y,view_width,view_height;
  bool configure(unsigned rw,unsigned rh) {
    if(rw<240 || rh<240 || rw>2048 || rh>2048)return false;
    raw_width=rw;raw_height=rh;width=rw>rh?rw:rh;height=rw>rh?rh:rw;
    view_width=width;view_height=width*540/960;
    if(view_height>height){view_height=height;view_width=height*960/540;}
    x=(width-view_width)/2;y=(height-view_height)/2;
    return view_width && view_height;
  }
  void raw(unsigned lx,unsigned ly,unsigned &rx,unsigned &ry) const {
    rx=lx;ry=ly;if(raw_height>raw_width){rx=raw_width-1-ly;ry=lx;}
  }
  bool portrait(unsigned rx,unsigned ry,unsigned &px,unsigned &py) const {
    if(rx>=raw_width || ry>=raw_height)return false;
    unsigned lx=rx,ly=ry;
    if(raw_height>raw_width){lx=ry;ly=raw_width-1-rx;}
    if(lx<x || lx>=x+view_width || ly<y || ly>=y+view_height)return false;
    // Inverse of the original rotate_portrait_to_panel, including pixel-center
    // selection so edge hit regions coincide with the sampled source pixels.
    py=(lx-x)*960/view_width;
    px=539-(ly-y)*540/view_height;
    return true;
  }
  bool touch_compatible(unsigned tw,unsigned th) const {
    return (tw==raw_width && th==raw_height) || (tw==raw_height && th==raw_width);
  }
  bool touch_portrait(unsigned tx,unsigned ty,unsigned tw,unsigned th,unsigned &px,unsigned &py) const {
    if(tx>=tw || ty>=th || !touch_compatible(tw,th))return false;
    unsigned rx=tx,ry=ty;
    // X4's panel exposes 800x480 memory while GT911 exposes its physical
    // 480x800 portrait coordinates. Match the existing clockwise panel map.
    if(tw!=raw_width){if(tw<th){rx=ty;ry=raw_height-1-tx;}else{rx=raw_width-1-ty;ry=tx;}}
    return portrait(rx,ry,px,py);
  }
};
