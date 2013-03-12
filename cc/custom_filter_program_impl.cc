// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cc/custom_filter_program_impl.h"

#include "cc/custom_filter_compiled_program.h"

namespace cc {

unsigned CustomFilterProgramImpl::id() const
{
    return m_id;
}

WebKit::WebCustomFilterProgramType CustomFilterProgramImpl::programType() const
{
    return m_programType;
}

WebKit::WebString CustomFilterProgramImpl::vertexShader() const
{
    return m_vertexShader;
}

WebKit::WebString CustomFilterProgramImpl::fragmentShader() const
{
    return m_fragmentShader;
}

void CustomFilterProgramImpl::refFromWebCustomFilterProgram()
{
    AddRef();
}
void CustomFilterProgramImpl::derefFromCustomFilterProgram()
{
    Release();
}

void CustomFilterProgramImpl::setCompiledProgram(CustomFilterCompiledProgram* compiledProgram)
{ 
    m_compiledProgram.reset(compiledProgram);
}

CustomFilterProgramImpl::~CustomFilterProgramImpl()
{
}

CustomFilterProgramImpl::CustomFilterProgramImpl(const WebKit::WebCustomFilterProgram* program)
    : m_id(program->id())
    , m_programType(program->programType())
    , m_vertexShader(program->vertexShader())
    , m_fragmentShader(program->fragmentShader())
{
}

}
