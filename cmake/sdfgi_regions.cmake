# Rects (x,y,w,h in a 1600x900 shot) the checks of cmake/sdfgi_tests.cmake compare, and their limits. sdfgi_outdoor's
# Game view in Play (camera still): the shaded floor by the red wall and by the green wall, the top of the red wall's
# inside (lit by the sunlit floor's bounce), the back wall under the overhang, open terrain on the left, the floor in the
# green wall's shade at both sun positions (sdfgi_sun), the back wall's left half (far from the Dynamic ball), terrain
# of a tile that streams in (sdfgi_stream: the right hillside). lightmap_room's Scene view
# (cmake/voxelgi_regions.cmake): its floor, its red wall, its Dynamic ball.
set(sg_red_floor 575,405,30,10)
set(sg_green_floor 945,370,25,10)
set(sg_red_wall 615,205,30,40)
set(sg_under_overhang 700,210,200,25)
set(sg_open_terrain 340,400,60,40)
set(sg_green_shade 997,400,40,10)
set(sg_far_from_ball 660,210,120,90)
set(sg_streamed_tile 1142,355,60,30)
set(sg_lm_floor ${vgi_lm_floor})
set(sg_lm_wall ${vgi_lm_wall})
set(sg_lm_ball ${vgi_lm_ball})
set(sg_popping_limit 2)  # mean difference of two consecutive frames along the path (without SDFGI: 1.4 at this pace)
set(sg_stream_limit 1)   # streamed-in terrain 10 frames in against settled (measured 0.6)
set(sg_player_limit 1)   # the player's Game view against the editor's
set(sg_player_rect 521,114,550,370,200,70) # inside the courtyard (the hosts clear to different backgrounds)
