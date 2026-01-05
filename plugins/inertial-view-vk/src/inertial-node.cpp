#include "inertial-node.hpp"
#include <wayfire/output.hpp>

inertial_node_t::inertial_node_t(wayfire_toplevel_view v, VulkanContext *c)
    : transformer_base_node_t(false), view(v), ctx(c) {}

wf::geometry_t inertial_node_t::get_bounding_box() {
  if (!view)
    return {0, 0, 0, 0};
  auto out = view->get_output();
  auto out_box = out ? out->get_layout_geometry() : wf::geometry_t{0, 0, 0, 0};
  auto g = view->get_geometry();
  return {(int)std::floor(phys_pos_global.x - out_box.x),
          (int)std::floor(phys_pos_global.y - out_box.y), g.width, g.height};
}

void inertial_node_t::gen_render_instances(
    std::vector<wf::scene::render_instance_uptr> &inst,
    wf::scene::damage_callback push, wf::output_t *out) {
  inst.push_back(std::make_unique<inertial_render_instance_t>(this, push, out));
}

void inertial_render_instance_t::render(
    const wf::scene::render_instruction_t &data) {
  if (!this->children.empty())
    this->children[0]->render(data);
}
