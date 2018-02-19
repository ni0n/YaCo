//  Copyright (C) 2017 The YaCo Authors
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "Ida.h"
#include "Events.hpp"

#define  MODULE_NAME "events"
#include "YaTypes.hpp"
#include "Pool.hpp"
#include "Hash.hpp"
#include "YaHelpers.hpp"
#include "HObject.hpp"
#include "Repository.hpp"
#include "Helpers.h"
#include "HVersion.hpp"
#include "IdaModel.hpp"
#include "IdaUtils.hpp"
#include "XmlModel.hpp"
#include "MemoryModel.hpp"
#include "DelegatingVisitor.hpp"
#include "IModel.hpp"
#include "XmlVisitor.hpp"
#include "Utils.hpp"
#include "IdaVisitor.hpp"
#include "IObjectListener.hpp"
#include "IdaModel.hpp"

#include <chrono>
#include <map>
#include <unordered_set>

#ifdef _MSC_VER
#   include <filesystem>
#else
#   include <experimental/filesystem>
#endif

namespace fs = std::experimental::filesystem;

namespace
{
    struct Ea
    {
        YaToolObjectId      id;
        YaToolObjectType_e  type;
        ea_t                ea;
    };

    bool operator<(const Ea& a, const Ea& b)
    {
        return std::make_pair(a.id, a.type) < std::make_pair(b.id, b.type);
    }

    struct Struc
    {
        tid_t id;
        ea_t  func_ea;
    };

    struct StrucMember
    {
        YaToolObjectId  parent_id;
        Struc           struc;
        ea_t            offset;
    };

    struct EnumMember
    {
        YaToolObjectId parent_id;
        enum_t         eid;
        const_t        mid;
    };

    using Eas           = std::set<Ea>;
    using Structs       = std::map<YaToolObjectId, Struc>;
    using StructMembers = std::map<YaToolObjectId, StrucMember>;
    using Enums         = std::map<YaToolObjectId, enum_t>;
    using EnumMembers   = std::map<YaToolObjectId, EnumMember>;
    using Segments      = std::set<ea_t>;

    struct Events
        : public IEvents
    {
        Events(IRepository& repo);

        // IEvents
        void touch_struc(tid_t struc_id) override;
        void touch_enum (enum_t enum_id) override;
        void touch_func (ea_t ea) override;
        void touch_code (ea_t ea) override;
        void touch_data (ea_t ea) override;
        void touch_ea   (ea_t ea) override;

        void save               () override;
        void update             () override;

        IRepository&    repo_;     
        Pool<qstring>   qpool_;

        Eas             eas_;
        Structs         strucs_;
        StructMembers   struc_members_;
        Enums           enums_;
        EnumMembers     enum_members_;
    };
}

Events::Events(IRepository& repo)
    : repo_(repo)
    , qpool_(3)
{
}

std::shared_ptr<IEvents> MakeEvents(IRepository& repo)
{
    return std::make_shared<Events>(repo);
}

namespace
{
    std::string to_hex(uint64_t ea)
    {
        char dst[2 + sizeof ea * 2];
        ea = swap(ea);
        const auto ref = binhex<sizeof ea, RemovePadding | HexaPrefix>(dst, &ea);
        return make_string(ref);
    }

    template<typename T>
    std::string to_string(const T& value)
    {
        std::stringstream stream;
        stream << value;
        return stream.str();
    }

    std::string make_frame_prefix(struc_t* frame)
    {
        const auto func_ea = get_func_by_frame(frame->id);
        return to_hex(func_ea) + ": stack ";
    }

    std::string make_struc_prefix(struc_t* struc)
    {
        if(struc->props & SF_FRAME)
            return make_frame_prefix(struc);

        qstring name;
        get_struc_name(&name, struc->id);
        if(name.empty())
            return std::string();

        return std::string("struc ") + name.c_str() + ": ";
    }

    std::string make_stackmember_prefix(struc_t* frame, member_t* member)
    {
        qstring name;
        get_member_name(&name, member->id);
        auto prefix = make_frame_prefix(frame);
        if(prefix.empty() || name.empty())
            return prefix;

        while(name[0] == ' ')
            name.remove(0, 1);
        prefix.resize(prefix.size() - 1); // remove last ' '
        return prefix + "." + name.c_str() + " ";
    }

    std::string make_member_prefix(struc_t* struc, member_t* member)
    {
        if(struc->props & SF_FRAME)
            return make_stackmember_prefix(struc, member);

        qstring name;
        get_member_name(&name, member->id);
        auto prefix = make_struc_prefix(struc);
        if(prefix.empty() || name.empty())
            return prefix;

        while(name[0] == ' ')
            name.remove(0, 1);
        prefix.resize(prefix.size() - 2); // remove last ": "
        return prefix + "." + name.c_str() + ": ";
    }

    std::string make_enum_prefix(enum_t eid)
    {
        qstring name;
        get_enum_name(&name, eid);
        if(name.empty())
            return std::string();

        return std::string("enum ") + name.c_str() + ": ";
    }

    std::string make_enum_member_prefix(enum_t eid, const_t mid)
    {
        qstring name;
        get_enum_member_name(&name, mid);
        auto prefix = make_enum_prefix(eid);
        if(prefix.empty() || name.empty())
            return std::string();

        prefix.resize(prefix.size() - 2); // remove last ": "
        return prefix + "." + name.c_str() + ": ";
    }

    std::string make_comment_prefix(ea_t ea)
    {
        if(ea == BADADDR)
            return std::string();

        auto struc = get_struc(ea);
        if(struc)
            return make_struc_prefix(struc);

        const auto member = get_member_by_id(ea, &struc);
        if(member)
            return make_member_prefix(struc, member);

        const auto idx = get_enum_idx(ea);
        if(idx != BADADDR)
            return make_enum_prefix(ea);

        const auto eid = get_enum_member_enum(ea);
        if(eid != BADADDR)
            return make_enum_member_prefix(eid, ea);

        if(!getseg(ea))
            return std::string();

        return to_hex(ea) + ": ";
    }

    void add_auto_comment(IRepository& repo, ea_t ea, const std::string& msg)
    {
        const auto prefix = make_comment_prefix(ea);
        if(!prefix.empty())
            repo.add_comment(prefix + msg);
    }

    void add_auto_comment(IRepository& repo, ea_t ea)
    {
        return add_auto_comment(repo, ea, "modified");
    }

    YaToolObjectId get_struc_stack_id(Events& ev, ea_t struc_id, ea_t func_ea)
    {
        if(func_ea != BADADDR)
            return hash::hash_stack(func_ea);

        const auto name = ev.qpool_.acquire();
        ya::wrap(&get_struc_name, *name, struc_id);
        return hash::hash_struc(ya::to_string_ref(*name));
    }

    struct IdAndType
    {
        YaToolObjectId     id;
        YaToolObjectType_e type;
    };

    IdAndType get_ea_type(ea_t ea)
    {
        const auto func = get_func(ea);
        if(func)
            return {hash::hash_function(ea), OBJECT_TYPE_FUNCTION};

        const auto flags = get_flags(ea);
        if(is_code(flags))
            return {hash::hash_ea(ea), OBJECT_TYPE_CODE};

        if(is_data(flags))
            return {hash::hash_ea(ea), OBJECT_TYPE_DATA};

        return {hash::hash_ea(ea), OBJECT_TYPE_UNKNOWN};
    }

    bool add_ea(Events& ev, YaToolObjectId id, YaToolObjectType_e type, ea_t ea)
    {
        return ev.eas_.emplace(Ea{id, type, ea}).second;
    }

    bool add_ea(Events& ev, ea_t ea)
    {
        const auto ctx = get_ea_type(ea);
        if(ctx.type == OBJECT_TYPE_UNKNOWN)
            return false;

        return add_ea(ev, ctx.id, ctx.type, ea);
    }

    void update_struc_member(Events& ev, struc_t* struc, const qstring& name, member_t* m)
    {
        const auto func_ea = get_func_by_frame(struc->id);
        const auto parent_id = func_ea != BADADDR ?
            hash::hash_stack(func_ea) :
            hash::hash_struc(ya::to_string_ref(name));
        const auto id = hash::hash_member(parent_id, m->soff);
        ev.struc_members_.emplace(id, StrucMember{parent_id, {struc->id, func_ea}, m->soff});
    }

    void update_enum_member(Events& ev, YaToolObjectId enum_id, enum_t eid, const_t cid)
    {
        const auto qbuf = ev.qpool_.acquire();
        ya::wrap(&::get_enum_member_name, *qbuf, cid);
        const auto id = hash::hash_enum_member(enum_id, ya::to_string_ref(*qbuf));
        ev.enum_members_.emplace(id, EnumMember{enum_id, eid, cid});
        add_auto_comment(ev.repo_, cid, "updated");
    }

    void update_enum(Events& ev, enum_t enum_id)
    {
        // check first whether enum_id is actually a member id
        const auto parent_id = get_enum_member_enum(enum_id);
        if(parent_id != BADADDR)
            enum_id = parent_id;

        const auto name = ev.qpool_.acquire();
        ya::wrap(&::get_enum_name, *name, enum_id);
        const auto id = hash::hash_enum(ya::to_string_ref(*name));
        ev.enums_.emplace(id, enum_id);
        ya::walk_enum_members(enum_id, [&](const_t cid, uval_t /*value*/, uchar /*serial*/, bmask_t /*bmask*/)
        {
            ::update_enum_member(ev, id, enum_id, cid);
        });
        add_auto_comment(ev.repo_, enum_id, "updated");
    }

    ea_t update_struc(Events& ev, tid_t struc_id)
    {
        add_auto_comment(ev.repo_, struc_id);
        const auto func_ea = get_func_by_frame(struc_id);
        const auto id = get_struc_stack_id(ev, struc_id, func_ea);
        ev.strucs_.emplace(id, Struc{struc_id, func_ea});

        const auto struc = get_struc(struc_id);
        if(!struc)
            return func_ea;

        const auto name = ev.qpool_.acquire();
        ya::wrap(&::get_struc_name, *name, struc->id);
        for(size_t i = 0; struc && i < struc->memqty; ++i)
            update_struc_member(ev, struc, *name, &struc->members[i]);

        return func_ea;
    }
}

void Events::touch_struc(tid_t struc_id)
{
    const auto func_ea = update_struc(*this, struc_id);
    if(func_ea != BADADDR)
        touch_func(func_ea);
}

void Events::touch_enum(enum_t enum_id)
{
    const auto parent_id = get_enum_member_enum(enum_id);
    add_auto_comment(repo_, enum_id);
    if(parent_id != BADADDR)
        enum_id = parent_id;
    update_enum(*this, enum_id);
}

void Events::touch_ea(ea_t ea)
{
    const auto ok = add_ea(*this, ea);
    if(ok)
        add_auto_comment(repo_, ea);
}

void Events::touch_func(ea_t ea)
{
    const auto ok = add_ea(*this, hash::hash_function(ea), OBJECT_TYPE_FUNCTION, ea);
    if(ok)
        add_auto_comment(repo_, ea);

    // add stack
    const auto frame = get_frame(ea);
    if(frame)
        update_struc(*this, frame->id);

    // add basic blocks
    const auto func = get_func(ea);
    if(!func)
        return;

    qflow_chart_t flow(nullptr, func, func->start_ea, func->end_ea, 0);
    for(const auto block : flow.blocks)
        add_ea(*this, hash::hash_ea(block.start_ea), OBJECT_TYPE_BASIC_BLOCK, block.start_ea);
}

void Events::touch_code(ea_t ea)
{
    const auto ok = add_ea(*this, hash::hash_ea(ea), OBJECT_TYPE_CODE, ea);
    if(ok)
        add_auto_comment(repo_, ea);
}

void Events::touch_data(ea_t ea)
{
    const auto ok = add_ea(*this, hash::hash_ea(ea), OBJECT_TYPE_DATA, ea);
    if(ok)
        add_auto_comment(repo_, ea);
}

namespace
{
    bool try_accept_struc(YaToolObjectId id, const Struc& struc, qstring& qbuf)
    {
        if(struc.func_ea != BADADDR)
            return get_func_by_frame(struc.id) == struc.func_ea;

        // on struc renames, as struc_id is still valid, we need to validate its id again
        ya::wrap(&get_struc_name, qbuf, struc.id);
        const auto got_id = hash::hash_struc(ya::to_string_ref(qbuf));
        const auto idx = get_struc_idx(struc.id);
        return id == got_id && idx != BADADDR;
    }

    void save_structs(Events& ev, IModelIncremental& model, IModelVisitor& visitor)
    {
        const auto qbuf = ev.qpool_.acquire();
        for(const auto p : ev.strucs_)
        {
            // if frame, we need to update parent function
            if(p.second.func_ea != BADADDR)
                model.accept_function(visitor, p.second.func_ea);
            if(try_accept_struc(p.first, p.second, *qbuf))
                model.accept_struct(visitor, p.second.func_ea, p.second.id);
            else if(p.second.func_ea == BADADDR)
                model.delete_struc(visitor, p.first);
            else
                model.delete_stack(visitor, p.first);
        }

        for(const auto p : ev.struc_members_)
        {
            const auto is_valid_parent = try_accept_struc(p.second.parent_id, p.second.struc, *qbuf);
            const auto struc = p.second.struc.func_ea != BADADDR ?
                get_frame(p.second.struc.func_ea) :
                get_struc(p.second.struc.id);
            const auto member = get_member(struc, p.second.offset);
            const auto id = hash::hash_member(p.second.parent_id, member ? member->soff : -1);
            const auto is_valid_member = p.first == id;
            if(is_valid_parent && is_valid_member)
                model.accept_struct(visitor, p.second.struc.func_ea, p.second.struc.id);
            else if(p.second.struc.func_ea == BADADDR)
                model.delete_struc_member(visitor, p.first);
            else
                model.delete_stack_member(visitor, p.first);
        }
    }

    void save_enums(Events& ev, IModelIncremental& model, IModelVisitor& visitor)
    {
        const auto qbuf = ev.qpool_.acquire();
        for(const auto p : ev.enums_)
        {
            // on renames, as enum_id is still valid, we need to validate its id again
            ya::wrap(&get_enum_name, *qbuf, p.second);
            const auto id = hash::hash_enum(ya::to_string_ref(*qbuf));
            const auto idx = get_enum_idx(p.second);
            if(idx == BADADDR || id != p.first)
                model.delete_enum(visitor, p.first);
            else
                model.accept_enum(visitor, p.second);
        }
        for(const auto p : ev.enum_members_)
        {
            // on renames, we need to check both ids
            ya::wrap(&get_enum_name, *qbuf, p.second.eid);
            const auto parent_id = hash::hash_enum(ya::to_string_ref(*qbuf));
            ya::wrap(&::get_enum_member_name, *qbuf, p.second.mid);
            const auto id = hash::hash_enum_member(parent_id, ya::to_string_ref(*qbuf));
            const auto parent = get_enum_member_enum(p.second.mid);
            if(parent == BADADDR || id != p.first || parent_id != p.second.parent_id)
                model.delete_enum_member(visitor, p.first);
            else
                model.accept_enum(visitor, p.second.eid);
        }
    }

    void save_func(IModelIncremental& model, IModelVisitor& visitor, YaToolObjectId id, ea_t ea)
    {
        const auto got = hash::hash_function(ea);
        const auto func = get_func(ea);
        if(got != id || !func)
        {
            model.delete_func(visitor, id);
            model.accept_ea(visitor, ea);
            return;
        }

        const auto ea_id = hash::hash_ea(ea);
        model.accept_function(visitor, ea);
        model.delete_code(visitor, ea_id);
        model.delete_data(visitor, ea_id);
    }

    void save_code(IModelIncremental& model, IModelVisitor& visitor, YaToolObjectId id, ea_t ea)
    {
        const auto got = hash::hash_ea(ea);
        const auto flags = get_flags(ea);
        const auto is_code_not_func = is_code(flags) && !get_func(ea);
        if(got != id || !is_code_not_func)
        {
            model.delete_code(visitor, id);
            model.accept_ea(visitor, ea);
            return;
        }

        model.accept_ea(visitor, ea);
        model.delete_func(visitor, hash::hash_function(ea));
        model.delete_data(visitor, got);
    }

    void save_data(IModelIncremental& model, IModelVisitor& visitor, YaToolObjectId id, ea_t ea)
    {
        const auto got = hash::hash_ea(ea);
        const auto flags = get_flags(ea);
        if(got != id || !is_data(flags))
        {
            model.delete_data(visitor, id);
            model.accept_ea(visitor, ea);
            return;
        }

        model.accept_ea(visitor, ea);
        model.delete_func(visitor, hash::hash_function(ea));
        model.delete_code(visitor, got);
    }

    void save_block(IModelIncremental& model, IModelVisitor& visitor, YaToolObjectId id, ea_t ea)
    {
        const auto got = hash::hash_ea(ea);
        const auto func = get_func(ea);
        if(got != id || !func)
        {
            model.delete_block(visitor, id);
            return;
        }

        model.accept_ea(visitor, ea);
    }

    void save_eas(Events& ev, IModelIncremental& model, IModelVisitor& visitor)
    {
        for(const auto p : ev.eas_)
            switch(p.type)
            {
                case OBJECT_TYPE_FUNCTION:      save_func(model, visitor, p.id, p.ea); break;
                case OBJECT_TYPE_CODE:          save_code(model, visitor, p.id, p.ea); break;
                case OBJECT_TYPE_DATA:          save_data(model, visitor, p.id, p.ea); break;
                case OBJECT_TYPE_BASIC_BLOCK:   save_block(model, visitor, p.id, p.ea); break;
                default:                        assert(false); break;
            }
    }

    std::string get_cache_folder_path()
    {
        std::string cache_folder_path = get_path(PATH_TYPE_IDB);
        remove_substring(cache_folder_path, fs::path(cache_folder_path).filename().string());
        cache_folder_path += "cache";
        return cache_folder_path;
    }

    void save(Events& ev)
    {
        IDA_LOG_INFO("Saving cache...");
        const auto time_start = std::chrono::system_clock::now();

        ModelAndVisitor db = MakeMemoryModel();
        db.visitor->visit_start();
        {
            const auto model = MakeIncrementalIdaModel();
            save_structs(ev, *model, *db.visitor);
            save_enums(ev, *model, *db.visitor);
            save_eas(ev, *model, *db.visitor);
        }
        db.visitor->visit_end();
        db.model->accept(*MakeXmlVisitor(get_cache_folder_path()));

        const auto time_end = std::chrono::system_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(time_end - time_start);
        IDA_LOG_INFO("Cache saved in %d seconds", static_cast<int>(elapsed.count()));
    }
}

void Events::save()
{
    ::save(*this);
    if(!repo_.commit_cache())
    {
        IDA_LOG_WARNING("An error occurred during YaCo commit");
        warning("An error occured during YaCo commit: please relaunch IDA");
    }
    eas_.clear();
    strucs_.clear();
    struc_members_.clear();
    enums_.clear();
    enum_members_.clear();
}

namespace
{
    struct DepCtx
    {
        DepCtx(const IModel& model)
            : model(model)
            , cache_prefix(fs::path(get_cache_folder_path()).filename())
            , xml_suffix(".xml")
        {
        }

        const IModel&                       model;
        const fs::path                      cache_prefix;
        const std::string                   xml_suffix;
        std::vector<std::string>            files;
        std::unordered_set<YaToolObjectId>  seen;
    };

    // will add id to file list if not already seen
    bool try_add_id(DepCtx& ctx, YaToolObjectType_e type, YaToolObjectId id)
    {
        // remember which ids have been seen already
        const auto inserted = ctx.seen.emplace(id).second;
        if(!inserted)
            return false;

        char hexname[17];
        to_hex<NullTerminate>(hexname, id);
        ctx.files.push_back((ctx.cache_prefix / get_object_type_string(type) / (hexname + ctx.xml_suffix)).generic_string());
        return true;
    }

    enum DepsMode
    {
        SKIP_DEPENDENCIES,
        USE_DEPENDENCIES,
    };

    bool must_add_dependencies(YaToolObjectType_e type)
    {
        // as we always recreate stacks & strucs, we always need every members
        return type == OBJECT_TYPE_STACKFRAME
            || type == OBJECT_TYPE_STRUCT
            || type == OBJECT_TYPE_ENUM;
    }

    void add_id_and_dependencies(DepCtx& ctx, YaToolObjectId id, DepsMode mode)
    {
        const auto hobj = ctx.model.get_object(id);
        if(!hobj.is_valid())
            return;

        // add this id to file list
        const auto ok = try_add_id(ctx, hobj.type(), id);
        if(!ok)
            return;

        hobj.walk_versions([&](const HVersion& hver)
        {
            // add parent id & its dependencies
            add_id_and_dependencies(ctx, hver.parent_id(), SKIP_DEPENDENCIES);
            if(mode != USE_DEPENDENCIES && !must_add_dependencies(hver.type()))
                return WALK_CONTINUE;
            hver.walk_xrefs([&](offset_t, operand_t, auto xref_id, auto)
            {
                // add xref id & its dependencies
                add_id_and_dependencies(ctx, xref_id, SKIP_DEPENDENCIES);
                return WALK_CONTINUE;
            });
            return WALK_CONTINUE;
        });
    }

    void SkipDelete(IModelVisitor* ptr)
    {
        UNUSED(ptr);
    }

    struct SkipVisitStartEndVisitor : public DelegatingVisitor
    {
        SkipVisitStartEndVisitor(IModelVisitor& next_visitor)
        {
            add_delegate(std::shared_ptr<IModelVisitor>(&next_visitor, &SkipDelete));
        }
        void visit_start() override {}
        void visit_end()   override {}
    };

    void add_missing_parents_from_deletions(DepCtx& deps, const std::unordered_set<YaToolObjectId> deleted)
    {
        if(deleted.empty())
            return;

        deps.model.walk_objects([&](YaToolObjectId id, const HObject& hobj)
        {
            if(!must_add_dependencies(hobj.type()))
                return WALK_CONTINUE;
            hobj.walk_versions([&](const HVersion& hver)
            {
                hver.walk_xrefs([&](offset_t, operand_t, auto xref_id, auto)
                {
                    if(deleted.count(xref_id))
                        add_id_and_dependencies(deps, id, USE_DEPENDENCIES);
                    return WALK_CONTINUE;
                });
                return WALK_CONTINUE;
            });
            return WALK_CONTINUE;
        });
    }

    void load_xml_files_to(IModelVisitor& visitor, const State& state)
    {
        visitor.visit_start();

        SkipVisitStartEndVisitor v(visitor);
        std::unordered_set<YaToolObjectId> deleted;
        for(const auto& it : state.deleted)
        {
            auto path = fs::path(it);
            path.replace_extension("");
            const auto idstr = path.filename().generic_string();
            const auto id = YaToolObjectId_From_String(idstr.data(), idstr.size());
            path.remove_filename();
            const auto typestr = path.filename();
            const auto type = get_object_type(typestr.generic_string().data());
            v.visit_start_deleted_object(type);
            v.visit_id(id);
            v.visit_end_deleted_object();
            deleted.emplace(id);
        }

        // state.updated contain only git modified files
        // i.e: if you apply a stack member on a basic block
        //      and the stack member is already in xml
        //      modified only contains one file, the basic block with one xref added
        // so we need to add all dependencies from this object
        // we do it by loading the full xml model
        // and add all parents recursively from all modified objects
        const auto files = [&]
        {
            // load all xml files into a new model which we will query
            const auto full = MakeMemoryModel();
            MakeXmlAllModel(".")->accept(*full.visitor);

            DepCtx deps(*full.model);

            // as we recreate strucs, stacks & enums
            // if one member is deleted, we must reapply parent
            add_missing_parents_from_deletions(deps, deleted);

            // load all modified objects
            const auto diff = MakeMemoryModel();
            MakeXmlFilesModel(state.updated)->accept(*diff.visitor);

            diff.model->walk_objects([&](auto id, const HObject& /*hobj*/)
            {
                // add this id & its dependencies
                add_id_and_dependencies(deps, id, USE_DEPENDENCIES);
                return WALK_CONTINUE;
            });
            return deps.files;
        }();
        MakeXmlFilesModel(files)->accept(v);
        visitor.visit_end();
    }
}

void Events::update()
{
    // update cache and export modifications to IDA
    {
        auto state = repo_.update_cache();
        const auto cache = fs::path(get_cache_folder_path()).filename();
        state.updated.erase(std::remove_if(state.updated.begin(), state.updated.end(), [&](const auto& item)
        {
            const auto p = fs::path(item);
            const auto it = p.begin();
            return it == p.end() || *it != cache;
        }), state.updated.end());
        load_xml_files_to(*MakeVisitorFromListener(*MakeIdaListener()), state);
    }

    // Let IDA apply modifications
    IDA_LOG_INFO("Running IDA auto-analysis...");
    const auto time_start = std::chrono::system_clock::now();
    const auto prev = inf.is_auto_enabled();
    inf.set_auto_enabled(true);
    auto_wait();
    inf.set_auto_enabled(prev);
    refresh_idaview_anyway();
    const auto time_end = std::chrono::system_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(time_end - time_start);
    IDA_LOG_INFO("Auto-analysis done in %d seconds", static_cast<int>(elapsed.count()));
}