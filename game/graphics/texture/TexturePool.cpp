#include <regex>
#include <algorithm>

#include "TexturePool.h"

#include "third-party/fmt/core.h"
#include "third-party/imgui/imgui.h"
#include "common/util/Timer.h"
#include "common/log/log.h"
#include "game/graphics/pipelines/opengl.h"
#include "common/util/Assert.h"
#include "game/graphics/texture/jak1_tpage_dir.h"

namespace {
const char empty_string[] = "";
const char* goal_string(u32 ptr, const u8* memory_base) {
  if (ptr == 0) {
    return empty_string;
  }
  return (const char*)(memory_base + ptr + 4);
}

}  // namespace

std::string GoalTexturePage::print() const {
  return fmt::format("Tpage id {} textures {} seg0 {} {} seg1 {} {} seg2 {} {}\n", id, length,
                     segment[0].size, segment[0].dest, segment[1].size, segment[1].dest,
                     segment[2].size, segment[2].dest);
}

u64 upload_to_gpu(const u8* data, u16 w, u16 h) {
  GLuint tex_id;
  glGenTextures(1, &tex_id);
  GLint old_tex;
  glGetIntegerv(GL_ACTIVE_TEXTURE, &old_tex);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, tex_id);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_INT_8_8_8_8_REV, data);
  glGenerateMipmap(GL_TEXTURE_2D);
  float aniso = 0.0f;
  glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &aniso);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, aniso);

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glActiveTexture(old_tex);
  return tex_id;
}

GpuTexture* TexturePool::give_texture(const TextureInput& in) {
  // const auto& it = m_loaded_textures.find(in.name);
  const auto existing = m_loaded_textures.lookup_or_insert(in.id);
  if (!existing.second) {
    // nothing references this texture yet.
    existing.first->tex_id = in.id;
    existing.first->w = in.w;
    existing.first->h = in.h;
    existing.first->is_common = in.common;
    existing.first->gpu_textures = {{in.gpu_texture, in.src_data}};
    existing.first->is_placeholder = false;
    *m_id_to_name.lookup_or_insert(in.id).first =
        fmt::format("{}/{}", in.debug_page_name, in.debug_name);
    return existing.first;
  } else {
    if (!existing.first->is_placeholder) {
      // two sources for texture. this is fine.
      ASSERT(!existing.first->gpu_textures.empty());
    } else {
      ASSERT(existing.first->gpu_textures.empty());
    }
    existing.first->is_placeholder = false;
    existing.first->w = in.w;
    existing.first->h = in.h;
    existing.first->gpu_textures.push_back({in.gpu_texture, in.src_data});
    existing.first->is_common = in.common;
    refresh_links(*existing.first);
    return existing.first;
  }
}

GpuTexture* TexturePool::give_texture_and_load_to_vram(const TextureInput& in, u32 vram_slot) {
  auto tex = give_texture(in);
  move_existing_to_vram(tex, vram_slot);
  return tex;
}

void TexturePool::move_existing_to_vram(GpuTexture* tex, u32 slot_addr) {
  ASSERT(!tex->is_placeholder);
  ASSERT(!tex->gpu_textures.empty());
  auto& slot = m_textures[slot_addr];
  if (std::find(tex->slots.begin(), tex->slots.end(), slot_addr) == tex->slots.end()) {
    tex->slots.push_back(slot_addr);
  }
  if (slot.source) {
    if (slot.source == tex) {
      // we already have it, no need to do anything
    } else {
      slot.source->remove_slot(slot_addr);
      slot.source = tex;
      slot.gpu_texture = tex->gpu_textures.front().gl;
    }
  } else {
    slot.source = tex;
    slot.gpu_texture = tex->gpu_textures.front().gl;
  }
}

void TexturePool::refresh_links(GpuTexture& texture) {
  u64 tex_to_use =
      texture.is_placeholder ? m_placeholder_texture_id : texture.gpu_textures.front().gl;

  for (auto slot : texture.slots) {
    auto& t = m_textures[slot];
    ASSERT(t.source == &texture);
    t.gpu_texture = tex_to_use;
  }

  for (auto slot : texture.mt4hh_slots) {
    for (auto& tex : m_mt4hh_textures) {
      if (tex.slot == slot) {
        tex.ref.gpu_texture = tex_to_use;
      }
    }
  }
}

void TexturePool::unload_texture(PcTextureId tex_id, u64 gpu_id) {
  auto* tex = m_loaded_textures.lookup_existing(tex_id);
  ASSERT(tex);
  if (tex->is_common) {
    ASSERT(false);
    return;
  }
  if (tex->is_placeholder) {
    fmt::print("trying to unload something that was already placholdered: {} {}\n",
               get_debug_texture_name(tex_id), tex->gpu_textures.size());
  }
  ASSERT(!tex->is_placeholder);
  auto it = std::find_if(tex->gpu_textures.begin(), tex->gpu_textures.end(),
                         [&](const auto& a) { return a.gl == gpu_id; });
  ASSERT(it != tex->gpu_textures.end());

  tex->gpu_textures.erase(it);
  if (tex->gpu_textures.empty()) {
    tex->is_placeholder = true;
  }
  refresh_links(*tex);
}

void GpuTexture::remove_slot(u32 slot) {
  auto it = std::find(slots.begin(), slots.end(), slot);
  ASSERT(it != slots.end());
  slots.erase(it);
}

void GpuTexture::add_slot(u32 slot) {
  ASSERT(std::find(slots.begin(), slots.end(), slot) == slots.end());
  slots.push_back(slot);
}

/*!
 * Handle a GOAL texture-page object being uploaded to VRAM.
 * The strategy:
 * - upload the texture-age to a fake 4MB VRAM, like the GOAL code would have done.
 * - "download" each texture in a reasonable format for the PC Port (currently RGBA8888)
 * - add this to the PC pool.
 *
 * The textures are scrambled around in a confusing way.
 *
 * NOTE: the actual conversion is currently done here, but this might be too slow.
 * We could store textures in the right format to begin with, or spread the conversion out over
 * multiple frames.
 */
void TexturePool::handle_upload_now(const u8* tpage, int mode, const u8* memory_base, u32 s7_ptr) {
  std::unique_lock<std::mutex> lk(m_mutex);
  // extract the texture-page object. This is just a description of the page data.
  GoalTexturePage texture_page;
  memcpy(&texture_page, tpage, sizeof(GoalTexturePage));

  bool has_segment[3] = {true, true, true};

  if (mode == -1) {
  } else if (mode == 2) {
    has_segment[0] = false;
    has_segment[1] = false;
  } else if (mode == -2) {
    has_segment[2] = false;
  } else if (mode == 0) {
    has_segment[1] = false;
    has_segment[2] = false;
  } else {
    // no reason to skip this, other than
    lg::error("TexturePool skipping upload now with mode {}.", mode);
    return;
  }

  // loop over all texture in the tpage and download them.
  for (int tex_idx = 0; tex_idx < texture_page.length; tex_idx++) {
    GoalTexture tex;
    if (texture_page.try_copy_texture_description(&tex, tex_idx, memory_base, tpage, s7_ptr)) {
      // each texture may have multiple mip levels.
      for (int mip_idx = 0; mip_idx < tex.num_mips; mip_idx++) {
        if (has_segment[tex.segment_of_mip(mip_idx)]) {
          PcTextureId current_id(texture_page.id, tex_idx);
          if (!m_id_to_name.lookup_existing(current_id)) {
            auto name = std::string(goal_string(texture_page.name_ptr, memory_base)) +
                        goal_string(tex.name_ptr, memory_base);
            *m_id_to_name.lookup_or_insert(current_id).first = name;
            m_name_to_id[name] = current_id;
          }

          auto& slot = m_textures[tex.dest[mip_idx]];

          if (slot.source) {
            if (slot.source->tex_id == current_id) {
              // we already have it, no need to do anything
            } else {
              slot.source->remove_slot(tex.dest[mip_idx]);
              slot.source = get_gpu_texture_for_slot(current_id, tex.dest[mip_idx]);
              ASSERT(slot.gpu_texture != (u64)-1);
            }
          } else {
            slot.source = get_gpu_texture_for_slot(current_id, tex.dest[mip_idx]);
            ASSERT(slot.gpu_texture != (u64)-1);
          }
        }
      }
    } else {
      // texture was #f, skip it.
    }
  }
}

void TexturePool::relocate(u32 destination, u32 source, u32 format) {
  std::unique_lock<std::mutex> lk(m_mutex);
  GpuTexture* src = lookup_gpu_texture(source);
  ASSERT(src);
  if (format == 44) {
    m_mt4hh_textures.emplace_back();
    m_mt4hh_textures.back().slot = destination;
    m_mt4hh_textures.back().ref.source = src;
    m_mt4hh_textures.back().ref.gpu_texture = src->gpu_textures.at(0).gl;
    src->mt4hh_slots.push_back(destination);
  } else {
    move_existing_to_vram(src, destination);
  }
}

GpuTexture* TexturePool::get_gpu_texture_for_slot(PcTextureId id, u32 slot) {
  auto it = m_loaded_textures.lookup_or_insert(id);
  if (!it.second) {
    GpuTexture& placeholder = *it.first;
    placeholder.tex_id = id;
    placeholder.is_placeholder = true;
    placeholder.slots.push_back(slot);

    // auto r = m_loaded_textures.insert({name, placeholder});
    m_textures[slot].gpu_texture = m_placeholder_texture_id;
    return it.first;
  } else {
    auto result = it.first;
    result->add_slot(slot);
    m_textures[slot].gpu_texture =
        result->is_placeholder ? m_placeholder_texture_id : result->gpu_textures.at(0).gl;
    return result;
  }
}

std::optional<u64> TexturePool::lookup_mt4hh(u32 location) {
  for (auto& t : m_mt4hh_textures) {
    if (t.slot == location) {
      if (t.ref.source) {
        return t.ref.gpu_texture;
      }
    }
  }
  return {};
}

TexturePool::TexturePool()
    : m_loaded_textures(get_jak1_tpage_dir()), m_id_to_name(get_jak1_tpage_dir()) {
  m_placeholder_data.resize(16 * 16);
  u32 c0 = 0xa0303030;
  u32 c1 = 0xa0e0e0e0;
  for (int i = 0; i < 16; i++) {
    for (int j = 0; j < 16; j++) {
      m_placeholder_data[i * 16 + j] = (((i / 4) & 1) ^ ((j / 4) & 1)) ? c1 : c0;
    }
  }
  m_placeholder_texture_id = upload_to_gpu((const u8*)(m_placeholder_data.data()), 16, 16);
}

void TexturePool::draw_debug_window() {
  int id = 0;
  int total_vram_bytes = 0;
  int total_textures = 0;
  int total_displayed_textures = 0;
  int total_uploaded_textures = 0;
  ImGui::InputText("texture search", m_regex_input, sizeof(m_regex_input));
  std::regex regex(m_regex_input[0] ? m_regex_input : ".*");

  for (size_t i = 0; i < m_textures.size(); i++) {
    auto& record = m_textures[i];
    total_textures++;
    if (record.source) {
      if (std::regex_search(get_debug_texture_name(record.source->tex_id), regex)) {
        ImGui::PushID(id++);
        draw_debug_for_tex(get_debug_texture_name(record.source->tex_id), record.source, i);
        ImGui::PopID();
        total_displayed_textures++;
      }
      if (!record.source->gpu_textures.empty()) {
        total_vram_bytes +=
            record.source->w * record.source->h * 4;  // todo, if we support other formats
      }

      total_uploaded_textures++;
    }
  }

  // todo mt4hh
  ImGui::Text("Total Textures: %d Uploaded: %d Shown: %d VRAM: %.3f MB", total_textures,
              total_uploaded_textures, total_displayed_textures,
              (float)total_vram_bytes / (1024 * 1024));
}

void TexturePool::draw_debug_for_tex(const std::string& name, GpuTexture* tex, u32 slot) {
  if (tex->is_placeholder) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8, 0.3, 0.3, 1.0));
  } else if (tex->gpu_textures.size() == 1) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3, 0.8, 0.3, 1.0));
  } else {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8, 0.8, 0.3, 1.0));
  }
  if (ImGui::TreeNode(fmt::format("{} {}", name, slot).c_str())) {
    ImGui::Text("P: %s sz: %d x %d", get_debug_texture_name(tex->tex_id).c_str(), tex->w, tex->h);
    if (!tex->is_placeholder) {
      ImGui::Image((void*)tex->gpu_textures.at(0).gl, ImVec2(tex->w, tex->h));
    } else {
      ImGui::Text("PLACEHOLDER");
    }

    ImGui::TreePop();
    ImGui::Separator();
  }
  ImGui::PopStyleColor();
}

PcTextureId TexturePool::allocate_pc_port_texture() {
  ASSERT(m_next_pc_texture_to_allocate < EXTRA_PC_PORT_TEXTURE_COUNT);
  return PcTextureId(get_jak1_tpage_dir().size() - 1, m_next_pc_texture_to_allocate++);
}

std::string TexturePool::get_debug_texture_name(PcTextureId id) {
  auto it = m_id_to_name.lookup_existing(id);
  if (it) {
    return *it;
  } else {
    return "???";
  }
}