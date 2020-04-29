/*
 * Copyright 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <utils/String8.h>

#include "ProgramCache.h"
#include "Program.h"
#include "Description.h"

namespace android {
// -----------------------------------------------------------------------------------------------


/*
 * A simple formatter class to automatically add the endl and
 * manage the indentation.
 */

class Formatter;
static Formatter& indent(Formatter& f);
static Formatter& dedent(Formatter& f);

class Formatter {
    String8 mString;
    int mIndent;
    typedef Formatter& (*FormaterManipFunc)(Formatter&);
    friend Formatter& indent(Formatter& f);
    friend Formatter& dedent(Formatter& f);
public:
    Formatter() : mIndent(0) {}

    String8 getString() const {
        return mString;
    }

    friend Formatter& operator << (Formatter& out, const char* in) {
        for (int i=0 ; i<out.mIndent ; i++) {
            out.mString.append("    ");
        }
        out.mString.append(in);
        out.mString.append("\n");
        return out;
    }
    friend inline Formatter& operator << (Formatter& out, const String8& in) {
        return operator << (out, in.string());
    }
    friend inline Formatter& operator<<(Formatter& to, FormaterManipFunc func) {
        return (*func)(to);
    }
};
Formatter& indent(Formatter& f) {
    f.mIndent++;
    return f;
}
Formatter& dedent(Formatter& f) {
    f.mIndent--;
    return f;
}

// -----------------------------------------------------------------------------------------------

ANDROID_SINGLETON_STATIC_INSTANCE(ProgramCache)

ProgramCache::ProgramCache() {
    // Until surfaceflinger has a dependable blob cache on the filesystem,
    // generate shaders on initialization so as to avoid jank.
    primeCache();
}

ProgramCache::~ProgramCache() {
}

void ProgramCache::primeCache() {
    uint32_t shaderCount = 0;
    uint32_t keyMask = Key::BLEND_MASK | Key::OPACITY_MASK |
                       Key::PLANE_ALPHA_MASK | Key::TEXTURE_MASK;
    // Prime the cache for all combinations of the above masks,
    // leaving off the experimental color matrix mask options.

    nsecs_t timeBefore = systemTime();
    for (uint32_t keyVal = 0; keyVal <= keyMask; keyVal++) {
        Key shaderKey;
        shaderKey.set(keyMask, keyVal);
        uint32_t tex = shaderKey.getTextureTarget();
        if (tex != Key::TEXTURE_OFF &&
            tex != Key::TEXTURE_EXT &&
            tex != Key::TEXTURE_2D) {
            continue;
        }
        Program* program = mCache.valueFor(shaderKey);
        if (program == NULL) {
            program = generateProgram(shaderKey);
            mCache.add(shaderKey, program);
            shaderCount++;
        }
    }
    nsecs_t timeAfter = systemTime();
    float compileTimeMs = static_cast<float>(timeAfter - timeBefore) / 1.0E6;
    ALOGD("shader cache generated - %u shaders in %f ms\n", shaderCount, compileTimeMs);
}

ProgramCache::Key ProgramCache::computeKey(const Description& description) {
    Key needs;
    needs.set(Key::TEXTURE_MASK,
            !description.mTextureEnabled ? Key::TEXTURE_OFF :
            description.mTexture.getTextureTarget() == GL_TEXTURE_EXTERNAL_OES ? Key::TEXTURE_EXT :
            description.mTexture.getTextureTarget() == GL_TEXTURE_2D           ? Key::TEXTURE_2D :
            Key::TEXTURE_OFF)
    .set(Key::PLANE_ALPHA_MASK,
            (description.mPlaneAlpha < 1) ? Key::PLANE_ALPHA_LT_ONE : Key::PLANE_ALPHA_EQ_ONE)
    .set(Key::BLEND_MASK,
            description.mPremultipliedAlpha ? Key::BLEND_PREMULT : Key::BLEND_NORMAL)
    .set(Key::OPACITY_MASK,
            description.mOpaque ? Key::OPACITY_OPAQUE : Key::OPACITY_TRANSLUCENT)
    .set(Key::COLOR_MATRIX_MASK,
            description.mColorMatrixEnabled ? Key::COLOR_MATRIX_ON :  Key::COLOR_MATRIX_OFF)
    .set(Key::WIDE_GAMUT_MASK,
            description.mIsWideGamut ? Key::WIDE_GAMUT_ON : Key::WIDE_GAMUT_OFF)
    .set(Key::BLUR_MASK,
            description.mBlur ? Key::BLUR_ON : Key::BLUR_OFF)
    .set(Key::FIRSTAPP_MASK,
            description.mFirstApp ? Key::FIRSTAPP_TRUE : Key::FIRSTAPP_FALSE);
    return needs;
}

String8 ProgramCache::generateVertexShader(const Key& needs) {
    Formatter vs;
    if (needs.isTexturing()) {
        vs  << "attribute vec4 texCoords;"
            << "varying vec2 outTexCoords;";
    }
    vs << "attribute vec4 position;"
       << "uniform mat4 projection;"
       << "uniform mat4 texture;"
       << "void main(void) {" << indent
       << "gl_Position = projection * position;";
    if (needs.isTexturing()) {
        vs << "outTexCoords = (texture * texCoords).st;";
    }
    vs << dedent << "}";
    return vs.getString();
}

String8 ProgramCache::generateFragmentShader(const Key& needs) {
    Formatter fs;
    if (needs.getTextureTarget() == Key::TEXTURE_EXT) {
        fs << "#extension GL_OES_EGL_image_external : require";
    }

    // default precision is required-ish in fragment shaders
    fs << "precision mediump float;";

    if (needs.getTextureTarget() == Key::TEXTURE_EXT) {
        fs << "uniform samplerExternalOES sampler;"
           << "varying vec2 outTexCoords;";
    } else if (needs.getTextureTarget() == Key::TEXTURE_2D) {
        fs << "uniform sampler2D sampler;"
           << "varying vec2 outTexCoords;";
    } else if (needs.getTextureTarget() == Key::TEXTURE_OFF) {
        fs << "uniform vec4 color;";
    }
    if (needs.hasPlaneAlpha()) {
        fs << "uniform float alphaPlane;";
    }
    if (needs.hasColorMatrix()) {
        fs << "uniform mat4 colorMatrix;";
    }
    if (needs.hasColorMatrix()) {
        // When in wide gamut mode, the color matrix will contain a color space
        // conversion matrix that needs to be applied in linear space
        // When not in wide gamut, we can simply no-op the transfer functions
        // and let the shader compiler get rid of them
        if (needs.isWideGamut()) {
            fs << R"__SHADER__(
                  float OETF_sRGB(const float linear) {
                      return linear <= 0.0031308 ?
                              linear * 12.92 : (pow(linear, 1.0 / 2.4) * 1.055) - 0.055;
                  }

                  vec3 OETF_sRGB(const vec3 linear) {
                      return vec3(OETF_sRGB(linear.r), OETF_sRGB(linear.g), OETF_sRGB(linear.b));
                  }

                  vec3 OETF_scRGB(const vec3 linear) {
                      return sign(linear.rgb) * OETF_sRGB(abs(linear.rgb));
                  }

                  float EOTF_sRGB(float srgb) {
                      return srgb <= 0.04045 ? srgb / 12.92 : pow((srgb + 0.055) / 1.055, 2.4);
                  }

                  vec3 EOTF_sRGB(const vec3 srgb) {
                      return vec3(EOTF_sRGB(srgb.r), EOTF_sRGB(srgb.g), EOTF_sRGB(srgb.b));
                  }

                  vec3 EOTF_scRGB(const vec3 srgb) {
                      return sign(srgb.rgb) * EOTF_sRGB(abs(srgb.rgb));
                  }
            )__SHADER__";
        } else {
            fs << R"__SHADER__(
                  vec3 OETF_scRGB(const vec3 linear) {
                      return linear;
                  }

                  vec3 EOTF_scRGB(const vec3 srgb) {
                      return srgb;
                  }
            )__SHADER__";
        }
    }
    fs << "uniform int rWidth;";
    fs << "uniform int rHeight;";
    if (needs.isBlur()) {
        fs << "uniform float iterator;"
           << "uniform float saturation;"
           << "uniform float sx;"
           << "uniform float bx;"
           << "uniform float sy;"
           << "uniform float by;";
    }
    fs << "void main(void) {" << indent;
    if (needs.isTexturing()) {
        fs << "gl_FragColor = texture2D(sampler, outTexCoords);";
    } else {
        fs << "gl_FragColor = color;";
    }
    if (needs.isBlur()) {
        fs << "vec2 resolution = vec2(1.0f / float(rWidth), 1.0f / float(rHeight));"
           << "vec2 pixelSize = resolution * 4.0f;"
           << "vec2 halfPixelSize = pixelSize / 2.0f;"
           << "vec2 dUV = (pixelSize.xy * vec2(iterator, iterator)) + halfPixelSize.xy;"
           << "vec3 cOut, cOut0, cOut1, cOut2, cOut3, cOut4;"
           << "float x1, y1, x2, y2;"
           << "float set = 0.45f;"
           << "x1 = outTexCoords.x - dUV.x;"
           << "x2 = outTexCoords.x + dUV.x;"
           << "y1 = outTexCoords.y - dUV.y;"
           << "y2 = outTexCoords.y + dUV.y;"
           << "if(x1 < sx) x1 = outTexCoords.x;"
           << "if(x2 > bx) x2 = outTexCoords.x;"
           << "if(y1 < sy) y1 = outTexCoords.y;"
           << "if(y2 > by) y2 = outTexCoords.y;"
           << "cOut = texture2D(sampler, outTexCoords).xyz;"
           << "cOut1 = texture2D(sampler, vec2(x1, y1)).xyz;"
           << "cOut2 = texture2D(sampler, vec2(x1, y2)).xyz;"
           << "cOut3 = texture2D(sampler, vec2(x2, y1)).xyz;"
           << "cOut4 = texture2D(sampler, vec2(x2, y2)).xyz;"
           << "cOut  = cOut + cOut1 + cOut2 + cOut3 + cOut4;"
           << "cOut *= 0.2f;"
           << "const vec3 W = vec3(0.2125, 0.7154, 0.0721);"
           << "vec3 intensity = vec3(dot(cOut.rgb, W));"
           << "cOut.rgb = mix(intensity, cOut.rgb, saturation);"
           << "gl_FragColor = vec4(cOut.xyz, 1.0f);";
    }
    if (needs.isOpaque()) {
        fs << "gl_FragColor.a = 1.0;";
    }
    if (needs.hasPlaneAlpha()) {
        // modulate the alpha value with planeAlpha
        if (needs.isPremultiplied()) {
            // ... and the color too if we're premultiplied
            fs << "gl_FragColor *= alphaPlane;";
        } else {
            fs << "gl_FragColor.a *= alphaPlane;";
        }
    }

    if (needs.hasColorMatrix()) {
        if (!needs.isOpaque() && needs.isPremultiplied()) {
            // un-premultiply if needed before linearization
            // avoid divide by 0 by adding 0.5/256 to the alpha channel
            fs << "gl_FragColor.rgb = gl_FragColor.rgb / (gl_FragColor.a + 0.0019);";
        }
        fs << "vec4 transformed = colorMatrix * vec4(EOTF_scRGB(gl_FragColor.rgb), 1);";
        // We assume the last row is always {0,0,0,1} and we skip the division by w
        fs << "gl_FragColor.rgb = OETF_scRGB(transformed.rgb);";
        if (!needs.isOpaque() && needs.isPremultiplied()) {
            // and re-premultiply if needed after gamma correction
            fs << "gl_FragColor.rgb = gl_FragColor.rgb * (gl_FragColor.a + 0.0019);";
        }
    }

    fs << dedent << "}";
    return fs.getString();
}

Program* ProgramCache::generateProgram(const Key& needs) {
    // vertex shader
    String8 vs = generateVertexShader(needs);

    // fragment shader
    String8 fs = generateFragmentShader(needs);

    Program* program = new Program(needs, vs.string(), fs.string());
    return program;
}

void ProgramCache::changeUniform(const Description& description) {
    Key needs(computeKey(description));
    Program* program = mCache.valueFor(needs);
    if (program == NULL || !needs.isBlur()) {
        return;
    }
    if (description.blurnum % 2 == 0) {
        glUniform1i(program->getUniform("blurnum1"), description.blurnum);
        glUniform1i(program->getUniform("blurnum2"), 0);
    } else {
        glUniform1i(program->getUniform("blurnum2"), description.blurnum);
        glUniform1i(program->getUniform("blurnum1"), 0);
    }
}

void ProgramCache::useProgram(const Description& description) {

    // generate the key for the shader based on the description
    Key needs(computeKey(description));

     // look-up the program in the cache
    Program* program = mCache.valueFor(needs);
    if (program == NULL) {
        // we didn't find our program, so generate one...
        nsecs_t time = -systemTime();
        program = generateProgram(needs);
        mCache.add(needs, program);
        time += systemTime();

        //ALOGD(">>> generated new program: needs=%08X, time=%u ms (%d programs)",
        //        needs.mNeeds, uint32_t(ns2ms(time)), mCache.size());
    }

    // here we have a suitable program for this description
    if (program->isValid()) {
        program->use();
        program->setUniforms(description);
        glUniform1i(program->getUniform("rHeight"), description.hwHeight);
        glUniform1i(program->getUniform("rWidth"), description.hwWidth);
        if (needs.isBlur()) {
            glUniform1f(program->getUniform("sx"), description.sx);
            glUniform1f(program->getUniform("bx"), description.bx);
            glUniform1f(program->getUniform("sy"), description.sy);
            glUniform1f(program->getUniform("by"), description.by);
            if (description.blurnum == 1) {
                glUniform1f(program->getUniform("iterator"), 0);
                glUniform1f(program->getUniform("saturation"), 1.0);
            } else if (description.blurnum == 2) {
                glUniform1f(program->getUniform("iterator"), 1);
                glUniform1f(program->getUniform("saturation"), 1.0);
            } else if (description.blurnum == 3) {
                glUniform1f(program->getUniform("iterator"), 2);
                glUniform1f(program->getUniform("saturation"), 1.0);
            } else if (description.blurnum == 4) {
                glUniform1f(program->getUniform("iterator"), 3);
                glUniform1f(program->getUniform("saturation"), 1.0);
            } else if (description.blurnum == 5) {
                glUniform1f(program->getUniform("iterator"), 4);
                glUniform1f(program->getUniform("saturation"), 1.0);
            } else if (description.blurnum == 6) {
                glUniform1f(program->getUniform("iterator"), 4);
                glUniform1f(program->getUniform("saturation"), 1.0);
            } else if (description.blurnum == 7) {
                glUniform1f(program->getUniform("iterator"), 5);
                glUniform1f(program->getUniform("saturation"), 2.0);
            }
        }
    }
}


} /* namespace android */
