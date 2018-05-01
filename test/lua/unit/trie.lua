-- Trie search tests

context("Trie search functions", function()
  local t = require "rspamd_trie"
  local logger = require "rspamd_logger"
  local patterns = {
    'test',
    'est',
    'he',
    'she',
    'str\1ing'
  }

  local function comparetables(t1, t2)
    if #t1 ~= #t2 then return false end
    for i=1,#t1 do
      if type(t1[i]) ~= type(t2[i]) then return false
      elseif type(t1[i]) == 'table' then
        if not comparetables(t1[i], t2[i]) then return false end
      elseif t1[i] ~= t2[i] then
        return false
      end
    end
    return true
  end

  local trie = t.create(patterns)

  local cases = {
    {'test', true, {{4, 1}, {4, 2}}},
    {'she test test', true, {{3, 4}, {3, 3}, {8, 1}, {8, 2}, {13, 1}, {13, 2}}},
    {'non-existent', false},
    {'str\1ing test', true, {{7, 5}, {12, 1}, {12, 2}}},
  }

  for i,c in ipairs(cases) do
    test("Trie search " .. i, function()
      local res = {}
      local function cb(idx, pos)
        table.insert(res, {pos, idx})

        return 0
      end

      ret = trie:match(c[1], cb)

      assert_equal(c[2], ret, tostring(c[2]) .. ' while matching ' .. c[1])

      if ret then
        table.sort(res, function(a, b) return a[2] > b[2] end)
        table.sort(c[3], function(a, b) return a[2] > b[2] end)
        local cmp = comparetables(res, c[3])
        assert_true(cmp, 'valid results for case: ' .. c[1] ..
                ' got: ' .. logger.slog('%s', res) .. ' expected: ' ..
                logger.slog('%s', c[3])
        )
      end
    end)
  end

end)
