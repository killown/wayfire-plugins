#pragma once

#include "vulkan-context.hpp"
#include <wayfire/toplevel-view.hpp>
#include <wayfire/view-transform.hpp>

class inertial_node_t : public wf::scene::transformer_base_node_t {
public:
  wf::pointf_t phys_pos_global = {0, 0};
  wf::pointf_t velocity = {0, 0};
  wf::pointf_t grab_off_global = {0, 0};
  bool is_moving = false;
  wayfire_toplevel_view view;
  VulkanContext *ctx;

  inertial_node_t(wayfire_toplevel_view v, VulkanContext *c);
  wf::geometry_t get_bounding_box() override;
  void gen_render_instances(std::vector<wf::scene::render_instance_uptr> &inst,
                            wf::scene::damage_callback push,
                            wf::output_t *out) override;
};

class inertial_render_instance_t
    : public wf::scene::transformer_render_instance_t<inertial_node_t> {
public:
  using transformer_render_instance_t::transformer_render_instance_t;
  void render(const wf::scene::render_instruction_t &data) override;
};
