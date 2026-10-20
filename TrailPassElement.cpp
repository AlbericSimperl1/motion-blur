#include "TrailPassElement.hpp"

CPointerTrailPass::CPointerTrailPass(SP<Render::ITexture> tex, std::vector<SCopy>&& copies) : m_tex(tex), m_copies(std::move(copies)) {
    ;
}

std::vector<UP<IPassElement>> CPointerTrailPass::draw() {
    std::vector<UP<IPassElement>> out;
    out.reserve(m_copies.size());
    for (const auto& c : m_copies) {
        CTexPassElement::SRenderData data;
        data.tex = m_tex;
        data.box = c.box;
        data.a   = c.a;
        out.emplace_back(makeUnique<CTexPassElement>(std::move(data)));
    }
    return out;
}

bool CPointerTrailPass::needsLiveBlur() {
    return false;
}

bool CPointerTrailPass::needsPrecomputeBlur() {
    return false;
}

const char* CPointerTrailPass::passName() {
    return "CPointerTrailPass";
}

ePassElementType CPointerTrailPass::type() {
    return EK_CUSTOM;
}
