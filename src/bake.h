#pragma once

// Geometry written again where a mapped item would have put it, as plain shapes. Open IFC Viewer won't
// select an element by clicking it when its body is many mapped items, as a merged asset's was, until it's
// been opened from the tree; a body of plain shapes it selects straight away.

#include "rigid.h"
#include "spf.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace bimpass {

class Baker {
  public:
    // gone says which items aren't in the file being written, such as a transparent part of an element, in
    // the envelopes file: they're left out
    Baker(const SpfFile& spf, Emitter& emitter, const std::function<bool(uint32_t)>& gone) : spf_(spf), emitter_(emitter), gone_(gone) {}

    // Whether every item of a representation can be moved, following maps inside it. If not, why_not says
    // what couldn't be.
    bool can_move(uint32_t representation);
    const std::string& why_not() const { return why_not_; }

    // Copies of a representation's items, moved by to, with the styles and colour maps of what they're copied
    // from. Adds them to items, and the input's instances they refer to, such as profiles and styles, to kept.
    void move(uint32_t representation, const Rigid& to, std::vector<uint32_t>& items, std::vector<uint32_t>& kept);

    // What a shape representation of these items, made by move, is: Tessellation, Brep, SweptSolid, CSG or
    // SolidModel. SolidModel, too, for a mixture of meshes and solids, which no type quite allows.
    std::string representation_type(const std::vector<uint32_t>& items) const;

  private:
    bool can_move_item(uint32_t item, int depth);
    void move_item(uint32_t item, const Rigid& to, int depth, std::vector<uint32_t>& out, std::vector<uint32_t>& kept);
    uint32_t moved_faces(uint32_t id, const Rigid& to, std::unordered_map<uint32_t, uint32_t>& done, std::vector<uint32_t>& kept);
    uint32_t moved_placement(std::string_view reference_text, const Rigid& to);
    void copy_followers(uint32_t from, uint32_t to, std::vector<uint32_t>& kept);
    void read_followers();
    uint32_t emit(const std::string& text, std::vector<uint32_t>& kept);

    const SpfFile& spf_;
    Emitter& emitter_;
    const std::function<bool(uint32_t)>& gone_;
    std::unordered_map<uint32_t, std::string> movable_;  // item, and why it can't be moved; empty if it can
    std::string why_not_;
    bool followers_read_ = false;
    std::unordered_map<uint32_t, std::vector<uint32_t>> followers_;  // item, and its styled items, colour maps and texture maps
};

}  // namespace bimpass
