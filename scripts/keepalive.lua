-- example dynamic request script which demonstrates changing
-- the connection request header to keep-alive
-- -------------------------------------------------------------

request = function()
    local headers = headers or wrk.headers
    headers["Connection"] = "keep-alive"
    return wrk.format(method, path, headers, body)
end
