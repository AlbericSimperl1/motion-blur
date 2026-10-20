#pragma once

#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <vector>

// One frame of the trail on one monitor: the fading copies, worked out before the pass is flushed. Drawn as an
// EK_CUSTOM element, so the renderer asks draw() for plain texture elements and draws those.
class CPointerTrailPass : public IPassElement {
  public:
    struct SCopy {
        CBox  box; // monitor-local pixels
        float a = 1.F;
    };

    CPointerTrailPass(SP<Render::ITexture> tex, std::vector<SCopy>&& copies);
    virtual ~CPointerTrailPass() = default;

    virtual std::vector<UP<IPassElement>> draw() override;
    virtual bool                          needsLiveBlur() override;
    virtual bool                          needsPrecomputeBlur() override;
    virtual const char*                   passName() override;
    virtual ePassElementType              type() override;

  private:
    SP<Render::ITexture> m_tex;
    std::vector<SCopy>   m_copies;
};
